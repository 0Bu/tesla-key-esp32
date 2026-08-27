// UDP Syslog forwarder. See syslog.hpp.
#include "syslog.hpp"
#include "config_blob.hpp"
#include "nvs_storage.hpp"
#include "task_config.hpp"
#include "rtos_guard.hpp"
#include "diag_crash.hpp"
#include "safe_mode.hpp"
#include "logic/syslog_policy.hpp"
#include "logic/syslog_start_gate.hpp"
#include "logic/bootlog.hpp"
#include "ping_probe.hpp"
#include <esp_app_desc.h>
#include <string>
#include <vector>
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
#include <atomic>
#include <cstring>
#include <cstdio>
#include <exception>
#include "net.hpp"
#include <string_view>

static const char* TAG = "syslog";

// The network seam (net.hpp) gates every network call below on a live link, whichever
// transport carries it.

struct SyslogMsg {
    char     text[256];
    uint16_t len;
};

// Depth chosen small on purpose: this device's binding memory limit is the largest
// *contiguous* free block (a few tens of KB steady-state — see the project AGENTS.md),
// and the queue is one contiguous allocation taken once at boot. 24 * ~258 B ~= 6.2 KB.
static constexpr UBaseType_t kQueueDepth = 24;

// Only syslog_send() consumes this publication. A queue is published exactly once, after the
// consumer task exists, and then lives until reboot. Startup failures clean up only unpublished
// local ownership, so a concurrent log hook can never retain a handle that is being deleted.
static std::atomic<QueueHandle_t> s_queue{nullptr};
static SemaphoreHandle_t s_status_mtx = nullptr;
static SyslogStatus      s_status;
// atomic: written in syslog_start (app_main), read by syslog_task and syslog_status (HTTP task).
static std::atomic<bool> s_configured{false};
static std::string       s_cfg_host;
static int               s_cfg_port   = 514;

// Persistent generation owner: a timeout stops the ping, but neither deletes nor reuses its
// callback state until the exact on_ping_end acknowledgement has arrived.
static tk::PingProbeControl s_ping{};

// Static because the task may begin running before xTaskCreate() returns. The task observes Run
// only after all boot-lifetime globals have been published; Cancel is a fail-closed escape for a
// future post-create startup failure. The current success tail contains only noexcept stores.
struct SyslogTaskStart {
    QueueHandle_t queue{nullptr};
    tk::SyslogStartGate gate{};
};

static SyslogTaskStart s_task_start{};

// Owns only resources that have not been published to syslog_send() or released to the task.
// Its destructor covers exceptions during configuration/startup without touching a live queue.
struct SyslogStartResources {
    QueueHandle_t queue{nullptr};
    SemaphoreHandle_t ping_done{nullptr};
    SemaphoreHandle_t status_mtx{nullptr};

    ~SyslogStartResources() noexcept {
        if (queue) vQueueDelete(queue);
        if (ping_done) vSemaphoreDelete(ping_done);
        if (status_mtx) vSemaphoreDelete(status_mtx);
    }

    void release() noexcept {
        queue = nullptr;
        ping_done = nullptr;
        status_mtx = nullptr;
    }
};

static void set_status(bool resolved, bool reachable, const std::string& error) {
    // RAII give — the s_status.error = error assignment can throw bad_alloc.
    tk::SemGuard g(s_status_mtx);
    if (!g) return;
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
    // RAII give — the std::string copies below can throw.
    tk::SemGuard g(s_status_mtx);
    if (g) {
        copy.resolved  = s_status.resolved;
        copy.reachable = s_status.reachable;
        copy.error     = s_status.error;
    }
    return copy;
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

    return tk::ping_probe_run(s_ping, cfg, pdMS_TO_TICKS(2200), pdMS_TO_TICKS(1600)) ==
           tk::PingProbeResult::Reply;
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
    // RFC 5424: <PRI>1 SP TIMESTAMP HOSTNAME APP PROCID MSGID SD SP MSG.
    // PRI is derived from the line's own esp_log level (logic/syslog_policy.hpp) rather than
    // hardcoded to user.info, so the collector's severity field actually discriminates —
    // see that header for why a constant PRI made `severity:error` useless.
    // HOSTNAME mirrors main.cpp's MDNS_HOSTNAME ("tesla-key-esp32") so entries
    // correlate with the device as seen over DHCP/mDNS.
    int pkt_len = std::snprintf(packet, sizeof(packet), "<%d>1 - tesla-key-esp32 - - - - %.*s",
                                 tk::syslog_pri_for_line(text, len),
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

// Replay the boot records ONCE per boot, the first time a destination resolves.
//
// WHY THIS IS NEEDED AT ALL. Everything logged before syslog_start() has nowhere to go — the queue
// does not exist yet and diag_log.cpp's hook drops the line. main.cpp already works around that for
// its own `BOOT reset_reason=` line by sampling early and logging late, but the CRASH record cannot
// use that trick: diag_crash_capture() runs before WiFi, its output is a multi-line report, and the
// in-RAM /diag ring that does hold it is erased by the very next restart. So the one record that
// explains an unattended reboot reached nothing that outlived the reboot.
//
// WHY NOT THROUGH THE QUEUE. syslog_send() is non-blocking and drops on a full queue — and at the
// moment of the first resolve the queue is full of the boot backlog, which is exactly when these
// lines would be dropped. They go straight down the socket instead.
//
// WHAT IS SENT. A build-identity line (version / elf sha / reset / safe-mode), which is the only
// way to tell WHICH firmware produced a log stream, and — only when the boot is NOTABLE — the crash
// records. A healthy boot sends one line and never spams the collector.
static void syslog_replay_boot(const struct sockaddr_in& dest, bool& replayed) {
    if (replayed) return;
    replayed = true;   // set FIRST: a throw below must not re-arm an unbounded retry every 10 s

    const tk::CrashInfo& ci = tk::diag_crash_info();

    std::vector<std::string> lines;
    const esp_app_desc_t* desc = esp_app_get_description();
    char run_sha[65] = {0};
    esp_app_get_elf_sha256(run_sha, sizeof(run_sha));
    lines.push_back(tk::build_boot_line(desc ? desc->version : "unknown", run_sha,
                                        tk::reset_reason_slug(ci.reset_code),
                                        tk::safe_mode_active()));
    tk::build_crash_log_lines(ci, lines);

    for (const std::string& l : lines) {
        int err = 0;
        // Best effort by design: this is UDP and the collector may not be listening yet. A failure
        // here must not disturb the resolve state the caller just established — the ordinary
        // forwarding path below owns that classification.
        (void)syslog_sendto(dest, l.c_str(), l.size(), &err);
    }
    ESP_LOGI(TAG, "replayed %u boot record(s) to the collector", (unsigned)lines.size());
}

static void syslog_task(void* raw_start) {
  try {
    auto* start = static_cast<SyslogTaskStart*>(raw_start);
    if (!start) {
        vTaskDelete(nullptr);
        return;
    }

    // xTaskCreate() may schedule this task immediately, before the creating task resumes. Do not
    // touch the queue or any other published runtime state until startup explicitly commits Run.
    for (;;) {
        const tk::SyslogStartAction action = start->gate.action();
        if (action == tk::SyslogStartAction::Run) break;
        if (action == tk::SyslogStartAction::Cancel) {
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(1);
    }
    QueueHandle_t const queue = start->queue;
    if (!queue) {
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in dest_addr{};
    bool resolved     = false;   // DNS resolved -> dest_addr valid -> forwarding lines
    bool reachable    = false;   // advisory probe result (see syslog_ping_host)
    bool logged_state = false;   // one-shot log of the current resolve outcome
    bool have_checked = false;   // false -> re-resolve immediately (boot / HARD send error)
    bool replayed     = false;   // the once-per-boot record replay (see syslog_replay_boot)
    bool send_failing = false;   // latch: forwarding is broken -> log the transition, not every line
    TickType_t last_check = 0;
    const TickType_t check_interval = pdMS_TO_TICKS(10000); // re-resolve + re-probe cadence

    while (true) {
      // Iteration-boundary containment (issue #204): getaddrinfo()/set_status() and the
      // std::string bookkeeping here can throw std::bad_alloc under heap pressure. An escape
      // would unwind into the FreeRTOS C task-entry trampoline → std::terminate → reboot; a
      // reboot loop also re-opens the car-poll window. Contain it, pause briefly so we don't
      // spin a tight error loop, and take the next queued line on the following iteration.
      try {
        if (!s_configured) {
            // Block until a line arrives, then drop it (nothing to forward) — no busy-spin.
            SyslogMsg msg;
            xQueueReceive(queue, &msg, portMAX_DELAY);
            continue;
        }

        if (!tk::net_is_up()) {
            if (resolved) { resolved = false; reachable = false; set_status(false, false, "network down"); }
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
                syslog_replay_boot(dest_addr, replayed);
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
        if (xQueueReceive(queue, &msg, pdMS_TO_TICKS(500)) == pdTRUE) {
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
      } catch (const std::exception& e) {
          ESP_LOGE(TAG, "syslog task iteration threw (%s) — pausing, will retry", e.what());
          vTaskDelay(pdMS_TO_TICKS(1000));
      } catch (...) {
          ESP_LOGE(TAG, "syslog task iteration threw (unknown) — pausing, will retry");
          vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }
  } catch (...) {
      ESP_LOGE(TAG, "syslog task boundary threw outside an iteration — stopping task");
      vTaskDelete(nullptr);
  }
}

static bool syslog_start_impl(NvsStorageAdapter& config_store) {
    tk::ConfigBlob cfg;
    tk::cfg_load(config_store, cfg);
    std::string uri = cfg.syslog_uri;
    // Trim surrounding whitespace (mirrors mqtt_ha_start's broker trim).
    size_t b = uri.find_first_not_of(" \t\r\n");
    size_t e = uri.find_last_not_of(" \t\r\n");
    uri = (b == std::string::npos) ? std::string{} : uri.substr(b, e - b + 1);

    std::string host;
    int port = 514;
    const bool configured = tk::syslog_target_parse(uri, host, port);
    s_cfg_host   = configured ? host : "";
    s_cfg_port   = configured ? port : 514;

    if (!configured) {
        s_configured.store(false);
        ESP_LOGI(TAG, "disabled (no server configured)");
        return true;   // nothing to start is a healthy outcome, not a failure
    }
    ESP_LOGI(TAG, "target set to %s:%d", s_cfg_host.c_str(), s_cfg_port);

    // Check every resource; on any failure unwind what we did allocate and fall back to the
    // disabled state so syslog_send() is a no-op and syslog_status() honestly reports
    // not-forwarding (an OPTIONAL subsystem must degrade visibly, never half-run).
    SyslogStartResources resources;
    resources.status_mtx = xSemaphoreCreateMutex();
    resources.ping_done  = xSemaphoreCreateBinary();
    resources.queue      = xQueueCreate(kQueueDepth, sizeof(SyslogMsg));
    if (!resources.status_mtx || !resources.ping_done || !resources.queue) {
        ESP_LOGE(TAG, "resource allocation failed — Syslog forwarding disabled (degraded)");
        s_configured.store(false, std::memory_order_release);
        return false;
    }

    // 6144: this task runs getaddrinfo() + raw socket()/sendto() directly on its own
    // stack (unlike esp-mqtt, whose socket work lives in an internal task). 4096 is
    // too thin for that call chain — mirrors syslog.cpp in the sibling
    // daikin-altherma-esp32 project, where this was measured.
    // Keep queue ownership local and invisible to the global log hook while task creation can
    // still fail. A newly scheduled task waits on the start gate and therefore cannot consume the
    // queue before the rest of the runtime state is committed.
    s_task_start.queue = resources.queue;
    if (!s_task_start.gate.begin()) {
        s_task_start.queue = nullptr;
        ESP_LOGE(TAG, "start gate was already used — Syslog forwarding disabled (degraded)");
        s_configured.store(false, std::memory_order_release);
        return false;
    }
    if (xTaskCreate(syslog_task, "syslog_task", 6144, &s_task_start,
                    tk::kPrioSyslog, nullptr) != pdPASS) {
        (void)s_task_start.gate.cancel();
        s_task_start.queue = nullptr;
        ESP_LOGE(TAG, "task creation failed — Syslog forwarding disabled (degraded)");
        s_configured.store(false, std::memory_order_release);
        return false;
    }

    // No operation from here through commit() can throw. Publish every boot-lifetime resource
    // before opening the task gate. A sender that observes the queue is guaranteed that it will
    // not be deleted for this boot, even on the defensive commit-failure path below.
    s_status_mtx = resources.status_mtx;
    s_ping.done = resources.ping_done;
    s_configured.store(true, std::memory_order_release);
    QueueHandle_t const queue = resources.queue;
    resources.release();
    s_queue.store(queue, std::memory_order_release);
    if (!s_task_start.gate.commit()) {
        // begin() succeeded and this startup path is the only writer, so this is defensive only.
        // Withdraw the sender publication and cancel a still-waiting task, but deliberately keep
        // every resource allocated until reboot: a sender may already hold the queue handle, or a
        // corrupted/unexpected Running state may already have let the task capture it. Leaking an
        // optional subsystem for this boot is bounded and safe; deleting here would re-open UAF.
        s_configured.store(false, std::memory_order_release);
        s_queue.store(nullptr, std::memory_order_release);
        (void)s_task_start.gate.cancel();
        ESP_LOGE(TAG, "start gate commit failed — Syslog forwarding disabled (degraded)");
        return false;
    }
    return true;
}

bool syslog_start(NvsStorageAdapter& config_store) {
    try {
        return syslog_start_impl(config_store);
    } catch (const std::exception& e) {
        s_configured.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "Syslog initialization threw (%s); forwarding disabled", e.what());
    } catch (...) {
        s_configured.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "Syslog initialization threw (unknown); forwarding disabled");
    }
    return false;
}

void syslog_send(const char* msg, size_t len) {
    QueueHandle_t const queue = s_queue.load(std::memory_order_acquire);
    if (!queue) return;

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
    xQueueSend(queue, &m, 0); // non-blocking
}
