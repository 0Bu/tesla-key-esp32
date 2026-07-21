// UDP Syslog forwarder. See syslog.hpp.
#include "syslog.hpp"
#include "nvs_storage.hpp"
#include "logic/syslog_policy.hpp"
#include "rtos_guard.hpp"
#include "task_config.hpp"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <esp_log.h>
#include <cstring>
#include <cstdio>
#include <exception>
#include <string_view>

static const char* TAG = "syslog";

// Defined in main.cpp: true only while the STA holds an IP. Mirrors mqtt_ha.cpp's
// own forward declaration — gates every network call below on a live link.
bool wifi_is_connected();

struct SyslogMsg {
    char     text[256];
    uint16_t len;
};

// Depth chosen small on purpose: this device's binding memory limit is the largest
// *contiguous* free block (a few tens of KB steady-state — see the project CLAUDE.md),
// and the queue is one contiguous allocation taken once at boot. 24 * ~258 B ~= 6.2 KB.
static constexpr UBaseType_t kQueueDepth = 24;

static QueueHandle_t     s_queue      = nullptr;
static SemaphoreHandle_t s_status_mtx = nullptr;
static SyslogStatus      s_status;
static bool              s_configured = false;
static std::string       s_cfg_host;
static int               s_cfg_port   = 514;

// File-scope so the control block + semaphore outlive any in-flight esp_ping session:
// the ping's internal thread is NOT joined by esp_ping_delete_session() and calls the
// callback unconditionally once started. A per-call stack frame would be a
// use-after-free if take() timed out first; file-scope storage removes the window (a
// stale give is drained at the next probe). Mirrors main.cpp's WdPing.
struct PingCtl { SemaphoreHandle_t done; uint32_t received; };
static PingCtl s_ping = { nullptr, 0 };

static void set_status(bool resolved, bool reachable, const std::string& error) {
    tk::MutexGuard lock(s_status_mtx);
    if (!lock) return;
    s_status.resolved  = resolved;
    s_status.reachable = reachable;
    s_status.error     = error;
}

SyslogStatus syslog_status() {
    SyslogStatus copy;
    copy.configured = s_configured;
    copy.host        = s_cfg_host;
    copy.port        = s_cfg_port;
    copy.resolved    = false;
    copy.reachable   = false;
    tk::MutexGuard lock(s_status_mtx);
    if (lock) {
        copy.resolved  = s_status.resolved;
        copy.reachable = s_status.reachable;
        copy.error     = s_status.error;
    }
    return copy;
}

static void syslog_on_ping_end(esp_ping_handle_t hdl, void* args) {
    auto* p = static_cast<PingCtl*>(args);
    uint32_t recv = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    p->received = recv;
    xSemaphoreGive(p->done);
}

// ADVISORY reachability probe — never a delivery gate (syslog is best-effort UDP, and
// a healthy collector routinely firewalls ICMP). ARP for a local-subnet host (L2, so
// it works even when the host drops ICMP), else an ICMP echo. Returns "could we
// confirm the host answers?" for the /status hint only; when it can't measure it
// returns false ("unverified"), and forwarding proceeds anyway.
static bool syslog_ping_host(const struct in_addr& ip) {
    struct netif* net = netif_default;
    if (net) {
        uint32_t mask = net->netmask.u_addr.ip4.addr;
        bool is_local = ((ip.s_addr & mask) == (net->ip_addr.u_addr.ip4.addr & mask));
        if (is_local) {
            // Provoke an ARP request with a 0-length datagram, then read the ARP cache back.
            int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            if (sock >= 0) {
                struct sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port   = htons(514);
                addr.sin_addr   = ip;
                char buf = 0;
                sendto(sock, &buf, 0, 0, (struct sockaddr*)&addr, sizeof(addr));
                close(sock);
            }
            vTaskDelay(pdMS_TO_TICKS(150)); // let the ARP reply land
            ip4_addr_t ipaddr; ipaddr.addr = ip.s_addr;
            struct eth_addr* eth_ret = nullptr;
            const ip4_addr_t* ip_ret = nullptr;
            if (etharp_find_addr(net, &ipaddr, &eth_ret, &ip_ret) >= 0) return true;
        }
    }

    // ICMP echo (remote host, or a local host not yet in the ARP cache).
    if (!s_ping.done) return false;   // probe not initialised -> can't measure -> advisory "unverified"
    ip_addr_t target{};
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = ip.s_addr;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count       = 2;
    cfg.timeout_ms  = 800;
    cfg.interval_ms = 200;

    esp_ping_callbacks_t cbs = {};
    cbs.cb_args     = &s_ping;
    cbs.on_ping_end = syslog_on_ping_end;

    esp_ping_handle_t hdl = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK || !hdl) return false;

    xSemaphoreTake(s_ping.done, 0);   // drain a stale give from a prior timed-out probe
    s_ping.received = 0;
    esp_ping_start(hdl);
    // A take() timeout here is harmless because s_ping is persistent (see above).
    xSemaphoreTake(s_ping.done, pdMS_TO_TICKS(2200));
    esp_ping_stop(hdl);
    vTaskDelay(pdMS_TO_TICKS(50));    // let the ping task context exit before deletion
    esp_ping_delete_session(hdl);
    return s_ping.received > 0;
}

// Frame one line as RFC 5424 and push it as a single UDP datagram.
enum class SendResult { Ok, Empty, SocketFailed, SendFailed };

// One UDP socket, reused for the syslog task's whole lifetime — NOT one socket()/
// close() per line. This device's own diag log routinely runs several lines/second
// (the BLE auto-pair retry loop alone logs ~8 lines per ~10s cycle, on top of every
// HTTP request's 2-3 lines), and UDP needs no connection state to reuse a socket
// across different destinations (sendto() takes the destination per call). Opening
// and closing a socket per line churns lwip's pcb pool on a device whose binding
// limit is the largest *contiguous* free block — under a burst that fragmentation
// cost a crash + auto-rollback in testing (v1.4.0 dev build, 2026-07-17).
static int s_sock = -1;

static bool syslog_ensure_socket() {
    if (s_sock >= 0) return true;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    return s_sock >= 0;
}

// `out_err` (optional) receives the errno CAPTURED AT THE FAILING CALL — and this
// errno decides whether the resolve throttle is cleared (logic/syslog_policy.hpp),
// so a wrong value costs a probe storm.
static SendResult syslog_sendto(const struct sockaddr_in& dest, const char* text, size_t len,
                                 int* out_err = nullptr) {
    if (out_err) *out_err = 0;
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' ')) {
        len--;
    }
    if (len == 0) return SendResult::Empty;

    if (!syslog_ensure_socket()) {
        if (out_err) *out_err = errno;
        return SendResult::SocketFailed;
    }

    char packet[320];
    // RFC 5424: <PRI=14 user.info>1 SP TIMESTAMP HOSTNAME APP PROCID MSGID SD SP MSG.
    // HOSTNAME mirrors main.cpp's MDNS_HOSTNAME ("tesla-key-esp32") so entries
    // correlate with the device as seen over DHCP/mDNS.
    int pkt_len = std::snprintf(packet, sizeof(packet), "<14>1 - tesla-key-esp32 - - - - %.*s",
                                 static_cast<int>(len), text);
    if (pkt_len <= 0) return SendResult::Ok;
    // snprintf returns the length it WOULD have written; clamp to what fits or sendto reads OOB.
    if (pkt_len > static_cast<int>(sizeof(packet)) - 1) pkt_len = static_cast<int>(sizeof(packet)) - 1;
    if (sendto(s_sock, packet, pkt_len, 0, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
        if (out_err) *out_err = errno;
        // The socket itself may be the broken part (e.g. its route died) — close it
        // so the next send re-creates a fresh one rather than retrying a dead fd forever.
        close(s_sock);
        s_sock = -1;
        return SendResult::SendFailed;
    }
    return SendResult::Ok;
}

// One place for "a send just failed", so the sendto and socket-creation paths can't
// drift apart. Two things this must NOT do, both learned the hard way on a sibling
// project's board that wedged itself (docs/ARCHITECTURE.md references the incident):
//   * Clear the resolve throttle on a TRANSIENT error (logic/syslog_policy.hpp). Only
//     a hard routing error justifies an immediate getaddrinfo() + ping probe; doing
//     that per failed line turns a chatty diag stream into a probe storm that runs
//     hardest exactly when the link is worst. Holding the throttle costs nothing —
//     the ordinary cadence still re-checks.
//   * Log per failure. A busy BLE poll can log several lines a second; logging every
//     syslog failure would itself flood /diag with syslog-failure spam. Log the
//     transition instead — one line when forwarding breaks, one when it recovers.
static void handle_send_failure(int err, const char* what, bool& resolved, bool& logged_state,
                                 bool& have_checked, bool& send_failing) {
    const bool hard = tk::syslog_error_is_hard(err);
    set_status(false, false, hard ? "Send failed" : "Send failed (transient)");
    // Decide from (hard, already-failing) — the once-per-outage re-probe is host-tested in
    // logic/syslog_policy.hpp. Clearing have_checked on EVERY hard failure (the old code) forced
    // getaddrinfo()+ping to re-run per queued line — a probe storm during an outage. Now the
    // immediate re-probe fires only on the FIRST hard failure; later ones just pause forwarding and
    // let check_interval govern re-checks.
    const tk::SendFailureActions act = tk::syslog_send_failure_actions(hard, send_failing);
    if (act.stop_forwarding) resolved = false;
    if (act.reprobe_once)  { have_checked = false; logged_state = false; }
    if (!send_failing) {
        send_failing = true;
        ESP_LOGW(TAG, "%s failed (error %d, %s) - forwarding paused",
                 what, err, hard ? "hard: re-resolving" : "transient: holding destination");
    }
}

static void syslog_task_impl() {
    struct sockaddr_in dest_addr{};
    bool resolved     = false;   // DNS resolved -> dest_addr valid -> forwarding lines
    bool reachable    = false;   // advisory probe result (see syslog_ping_host)
    bool logged_state = false;   // one-shot log of the current resolve outcome
    bool have_checked = false;   // false -> re-resolve immediately (boot / HARD send error)
    bool send_failing = false;   // latch: forwarding is broken -> log the transition, not every line
    TickType_t last_check = 0;
    const TickType_t check_interval = pdMS_TO_TICKS(10000); // re-resolve + re-probe cadence

    while (true) {
        if (!s_configured) {
            // Block until a line arrives, then drop it (nothing to forward) — no busy-spin.
            SyslogMsg msg;
            xQueueReceive(s_queue, &msg, portMAX_DELAY);
            continue;
        }

        if (!wifi_is_connected()) {
            if (resolved) { resolved = false; reachable = false; set_status(false, false, "WiFi disconnected"); }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Throttle the resolve+probe to check_interval. NOTE: gate on have_checked, NOT
        // on !resolved — a persistently failing DNS/host must not re-run
        // getaddrinfo()+ping every loop (that churns ping-session heap, a
        // fragmentation risk on this device's tight contiguous-block budget).
        TickType_t now = xTaskGetTickCount();
        if (!have_checked || (now - last_check >= check_interval)) {
            last_check = now;
            have_checked = true;

            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            char port_str[16];
            std::snprintf(port_str, sizeof(port_str), "%d", s_cfg_port);
            int err = getaddrinfo(s_cfg_host.c_str(), port_str, &hints, &res);
            if (err == 0 && res != nullptr) {
                std::memcpy(&dest_addr, res->ai_addr, sizeof(struct sockaddr_in));
                freeaddrinfo(res);
                resolved  = true;                                  // DNS ok -> forward regardless
                reachable = syslog_ping_host(dest_addr.sin_addr);  // advisory only
                set_status(true, reachable, "");
                if (!logged_state) {
                    char ip_str[32];
                    inet_ntop(AF_INET, &dest_addr.sin_addr, ip_str, sizeof(ip_str));
                    ESP_LOGI(TAG, "forwarding to %s (%s), reachable=%s",
                             s_cfg_host.c_str(), ip_str, reachable ? "yes" : "no-ping-reply");
                    logged_state = true;
                }
            } else {
                resolved = false; reachable = false;
                set_status(false, false, "DNS lookup failed");
                if (!logged_state) {
                    ESP_LOGW(TAG, "DNS lookup failed for %s (error %d)", s_cfg_host.c_str(), err);
                    logged_state = true;
                }
            }
        }

        // Forward one queued line while a destination is resolved. Delivery is gated
        // on DNS only (resolved), never on the advisory reachability probe.
        SyslogMsg msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (resolved) {
                int err = 0;
                switch (syslog_sendto(dest_addr, msg.text, msg.len, &err)) {
                    case SendResult::Ok:
                        if (send_failing) {   // first line through after an outage
                            ESP_LOGI(TAG, "forwarding recovered");
                            send_failing = false;
                        }
                        break;
                    case SendResult::Empty:   // nothing to send — neither success nor failure
                        break;
                    // Whether this clears the resolve throttle now depends on WHICH
                    // error it was (logic/syslog_policy.hpp), not merely that one occurred.
                    case SendResult::SendFailed:
                        handle_send_failure(err, "sendto", resolved, logged_state,
                                             have_checked, send_failing);
                        break;
                    case SendResult::SocketFailed:
                        handle_send_failure(err, "socket creation", resolved, logged_state,
                                             have_checked, send_failing);
                        break;
                }
            }
        }
    }
}

static void syslog_task(void*) {
    for (;;) {
        try {
            syslog_task_impl();
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "forwarder task threw (%s); restarting task state", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "forwarder task threw (unknown); restarting task state");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void syslog_cleanup_start_failure() noexcept {
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = nullptr;
    }
    if (s_ping.done) {
        vSemaphoreDelete(s_ping.done);
        s_ping.done = nullptr;
    }
    if (s_status_mtx) {
        vSemaphoreDelete(s_status_mtx);
        s_status_mtx = nullptr;
    }
    s_configured = false;
}

static bool syslog_start_impl(NvsStorageAdapter& config_store) {
    std::string uri = CONFIG_TESLA_SYSLOG_SERVER;
    config_store.load_str("syslog_uri", uri);
    // Trim surrounding whitespace (mirrors mqtt_ha_start's broker trim).
    size_t b = uri.find_first_not_of(" \t\r\n");
    size_t e = uri.find_last_not_of(" \t\r\n");
    uri = (b == std::string::npos) ? std::string{} : uri.substr(b, e - b + 1);

    std::string host;
    int port = 514;
    s_configured = tk::syslog_target_parse(uri, host, port);
    s_cfg_host   = s_configured ? host : "";
    s_cfg_port   = s_configured ? port : 514;

    if (!s_configured) {
        ESP_LOGI(TAG, "disabled (no server configured)");
        return true;
    }
    ESP_LOGI(TAG, "target set to %s:%d", s_cfg_host.c_str(), s_cfg_port);

    s_status_mtx = xSemaphoreCreateMutex();
    s_ping.done  = xSemaphoreCreateBinary();
    s_queue      = xQueueCreate(kQueueDepth, sizeof(SyslogMsg));
    if (!s_status_mtx || !s_ping.done || !s_queue) {
        ESP_LOGE(TAG, "failed to allocate Syslog mutex, ping semaphore, or queue");
        syslog_cleanup_start_failure();
        return false;
    }

    // 6144: this task runs getaddrinfo() + raw socket()/sendto() directly on its own
    // stack (unlike esp-mqtt, whose socket work lives in an internal task). 4096 is
    // too thin for that call chain — mirrors syslog.cpp in the sibling
    // daikin-altherma-esp32 project, where this was measured.
    if (xTaskCreate(syslog_task, "syslog_task", 6144, nullptr, tk::kPrioSyslog,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "failed to create Syslog forwarder task");
        syslog_cleanup_start_failure();
        return false;
    }
    return true;
}

bool syslog_start(NvsStorageAdapter& config_store) {
    try {
        return syslog_start_impl(config_store);
    } catch (const std::exception& e) {
        syslog_cleanup_start_failure();
        ESP_LOGE(TAG, "Syslog initialization threw (%s); forwarding disabled", e.what());
    } catch (...) {
        syslog_cleanup_start_failure();
        ESP_LOGE(TAG, "Syslog initialization threw (unknown); forwarding disabled");
    }
    return false;
}

void syslog_send(const char* msg, size_t len) {
    if (!s_queue) return;

    // Loop guard: this module's own diagnostics (ESP_LOGx(TAG, ...) above) carry the
    // "syslog:" tag esp_log renders into every line, and diag_log.cpp's capture hook
    // calls this function for ALL captured output — including this module's own. A
    // substring match, not a prefix, since the rendered line is
    // "I (12345) syslog: message", not "syslog: message". Without this, a "send
    // failed" line would itself be queued for sending, and (while failing) logged
    // again on the next attempt — feeding the exact storm handle_send_failure()
    // above exists to avoid.
    std::string_view sv(msg, len);
    if (sv.find("syslog:") != std::string_view::npos) {
        return;
    }

    SyslogMsg m;
    if (len >= sizeof(m.text)) {
        len = sizeof(m.text) - 1;
    }
    std::memcpy(m.text, msg, len);
    m.text[len] = '\0';
    m.len = static_cast<uint16_t>(len);
    xQueueSend(s_queue, &m, 0); // non-blocking
}
