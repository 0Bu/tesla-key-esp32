#include "ota_transport.hpp"
#include "ota_http.hpp"
#include "ota_recovery_memory.hpp"
#include <esp_tls.h>
#include <esp_tls_errors.h>
#include <esp_crt_bundle.h>
#include <algorithm>
#include <climits>
#include <cstring>

#if defined(ESP_PLATFORM)
#include <mbedtls/ssl.h>
// Refuse source/profile drift rather than claiming the record screen covers another allocator
// or TLS buffer configuration. Host fixtures do not stand in for this actual target binding.
#if !defined(CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC) || defined(CONFIG_MBEDTLS_DYNAMIC_BUFFER) || \
    defined(MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH) || defined(MBEDTLS_SSL_DTLS_CONNECTION_ID) || \
    !defined(MBEDTLS_SSL_PROTO_TLS1_2) || defined(MBEDTLS_SSL_PROTO_TLS1_3) || defined(MBEDTLS_SSL_PROTO_DTLS)
#error "OTA record-capacity policy requires the reviewed fixed INTERNAL TLS1.2 profile"
#endif
static_assert(MBEDTLS_SSL_IN_CONTENT_LEN == tk::kOtaTlsInContentB,
              "OTA input record bound differs from the actual TLS profile");
static_assert(MBEDTLS_SSL_OUT_CONTENT_LEN == tk::kOtaTlsOutContentB,
              "OTA output record bound differs from the actual TLS profile");
#endif

namespace tk::ota {
#if defined(F145_HOST_TEST_HOOKS)
// Host-only deterministic scheduling seam; absent from the compile-only IDF probe.
void host_before_cleanup_publish(Job&, std::uint32_t observed) noexcept;
#endif
namespace {
struct Connection {
    esp_tls_t* tls{nullptr};
    std::array<char,254> hostname{};
    esp_tls_cfg_t cfg{};
};
Phase phase_of(std::uint32_t word) noexcept { return static_cast<Phase>(word & kPhaseMask); }
bool closing(const Job& j) noexcept {
    return (j.control.load(std::memory_order_acquire) & (kRetired|kClose)) != 0;
}
int timeout(const Job& j, const NetRuntime& r) noexcept {
    const auto now = r.now_ms(r.context);
    if (now < j.started_ms || now >= j.deadline_ms) return 0;
    return static_cast<int>(std::min<std::uint64_t>(INT_MAX,j.deadline_ms-now));
}
// Only worker-owned data is touched in Reading. Release publication is a single CAS against
// the retirement/close bits, so a late SDK completion cannot publish after retirement.
bool publish(Job& j, NetResult result, std::size_t bytes, const NetRuntime& r) noexcept {
    if (bytes > j.block.size()) return false;
    j.result = result; j.bytes = bytes;
    std::uint32_t expected = static_cast<std::uint32_t>(Phase::Reading);
    if (!j.control.compare_exchange_strong(expected,static_cast<std::uint32_t>(Phase::Full),
            std::memory_order_acq_rel,std::memory_order_acquire)) return false;
    for (;;) {
        const auto word = j.control.load(std::memory_order_acquire);
        const auto phase = phase_of(word);
        if (phase == Phase::Reading) return (word & (kRetired|kClose)) == 0;
        // Retired Full is unborrowable. Borrowed remains charged until the writer returns.
        if ((word & (kRetired|kClose)) && phase != Phase::Borrowed) return false;
        r.pause_ms(r.context,1);
    }
}
bool send_piece(Job& j, esp_tls_t* tls, const char* text, std::size_t count,
                const NetRuntime& r) {
    std::size_t sent = 0;
    while (sent < count) {
        if (closing(j) || timeout(j,r) == 0) return false;
        const auto n = esp_tls_conn_write(tls,text+sent,count-sent);
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            r.pause_ms(r.context,1); continue;
        }
        if (n <= 0 || static_cast<std::size_t>(n) > count-sent) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}
NetResult transfer(Job& j, const NetRuntime& r, Connection& connection) {
    auto*& tls = connection.tls;
    http_parser_url url{};
    if (!valid_https_url(j.url.data(),url)) return NetResult::InvalidUrl;
    const auto host = url.field_data[UF_HOST];
    auto& hostname = connection.hostname;
    std::memcpy(hostname.data(),j.url.data()+host.off,host.len);
    const auto pre_init_largest = r.largest_internal(r.context);
    memory_checkpoint("tls_pre_init", j.generation, j.started_ms, pre_init_largest);
    if (!tk::ota_recovery_memory_affordable(pre_init_largest,
            tk::ota_recovery_required_bytes(tk::OtaRecoveryUse::TlsAdmission))) return NetResult::NoMemory;
    if (closing(j) || timeout(j,r) == 0) return NetResult::Deadline;
    tls = esp_tls_init();
    if (!tls) return NetResult::NoMemory;
    // The ordinary-entry R was checked before operation allocation. This distinct fixed-record
    // screen is refreshed after context allocation; passing it does not reserve a TLS budget.
    const auto post_context_largest = r.largest_internal(r.context);
    memory_checkpoint("tls_post_context", j.generation, j.started_ms, post_context_largest);
    if (!tk::ota_recovery_memory_affordable(post_context_largest,
            tk::ota_recovery_required_bytes(tk::OtaRecoveryUse::TlsAdmission))) return NetResult::NoMemory;
    const int remaining = timeout(j,r);
    if (closing(j) || remaining == 0) return NetResult::Deadline;
    auto& cfg = connection.cfg;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.common_name = hostname.data(); // Same original name for certificate validation and SNI.
    cfg.skip_common_name = false;
    cfg.timeout_ms = remaining; // Best effort; coordinator is independent of an SDK call.
    const int port = (url.field_set & (1u<<UF_PORT)) ? url.port : 443;
    const int connected = esp_tls_conn_new_sync(hostname.data(),host.len,port,&cfg,tls);
    memory_checkpoint(connected == 1 ? "tls_connected" : "tls_connect_failed",
                      j.generation, j.started_ms);
    if (connected != 1)
        return NetResult::NetworkError; // An arbitrary TLS error is not proof of OOM.
    if (closing(j) || timeout(j,r) == 0) return NetResult::Deadline;
    const auto send = [&](const char* p,std::size_t n) { return send_piece(j,tls,p,n,r); };
    const auto literal = [&](const char* p) { return send(p,std::strlen(p)); };
    if (!literal("GET ")) return NetResult::NetworkError;
    const auto path = url.field_data[UF_PATH];
    if (!(path.len ? send(j.url.data()+path.off,path.len) : literal("/"))) return NetResult::NetworkError;
    if (url.field_set & (1u<<UF_QUERY)) {
        const auto query = url.field_data[UF_QUERY];
        if (!literal("?") || !send(j.url.data()+query.off,query.len)) return NetResult::NetworkError;
    }
    if (!literal(" HTTP/1.1\r\nHost: ")) return NetResult::NetworkError;
    const bool ipv6 = std::memchr(hostname.data(),':',host.len) != nullptr;
    if ((ipv6 && !literal("[")) || !send(hostname.data(),host.len) ||
        (ipv6 && !literal("]"))) return NetResult::NetworkError;
    if (url.field_set & (1u<<UF_PORT)) {
        const auto port_text = url.field_data[UF_PORT];
        if (!literal(":") || !send(j.url.data()+port_text.off,port_text.len)) return NetResult::NetworkError;
    }
    if (!literal("\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n"))
        return NetResult::NetworkError;
    for (;;) {
        if (closing(j) || timeout(j,r) == 0) return NetResult::Deadline;
        const auto n = esp_tls_conn_read(tls,j.block.data(),j.block.size());
        if (closing(j) || timeout(j,r) == 0) return NetResult::Deadline;
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            r.pause_ms(r.context,1); continue;
        }
        if (n == 0) return NetResult::Eof;
        if (n < 0 || static_cast<std::size_t>(n) > j.block.size()) return NetResult::NetworkError;
        if (!publish(j,NetResult::Data,static_cast<std::size_t>(n),r)) return NetResult::Deadline;
    }
}
} // namespace

void run_network_worker(Job& j, const NetRuntime& r) noexcept {
    // This function intentionally has no writer/gate/status reference. A target wrapper may
    // self-delete only after this returns, with no Job-referencing destructor after the Ack.
    for (;;) {
        const auto word = j.control.load(std::memory_order_acquire);
        if (word & kRetired) {
            if (phase_of(word) == Phase::Borrowed) { r.pause_ms(r.context,1); continue; }
            auto expected = word;
            const auto ack = (word & ~kPhaseMask) | static_cast<std::uint32_t>(Phase::Acked);
            if (j.control.compare_exchange_strong(expected,ack,std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
                return; // Successful Ack CAS is the last Job access on this path.
            continue;
        }
        if (phase_of(word) != Phase::Requested) { r.pause_ms(r.context,1); continue; }
        std::uint32_t expected = static_cast<std::uint32_t>(Phase::Requested);
        if (!j.control.compare_exchange_strong(expected,static_cast<std::uint32_t>(Phase::Reading),
                std::memory_order_acq_rel,std::memory_order_acquire)) continue;
        Connection connection{}; // Hostname/config remain alive through TLS destruction.
        NetResult result = NetResult::NetworkError;
        bool cleanup_ok = true;
        try {
            result = transfer(j,r,connection);
            if (!closing(j)) (void)publish(j,result,0,r);
        } catch (...) {
            // SDK C calls do not normally throw; the C++ task boundary still contains them.
            if (!closing(j)) (void)publish(j,NetResult::NetworkError,0,r);
        }
        // Only this worker ever closes/frees this TLS context; it may block here indefinitely.
        if (connection.tls) {
            try { cleanup_ok = esp_tls_conn_destroy(connection.tls) == 0; }
            catch (...) { cleanup_ok = false; }
            connection.tls = nullptr; // destroy consumes it even on a socket-close error.
            memory_checkpoint(cleanup_ok ? "tls_destroyed" : "tls_destroy_failed",
                              j.generation, j.started_ms); // Before the final Ack/CleanupFailed CAS.
        }
        auto observed = j.control.load(std::memory_order_acquire);
        for (;;) {
            if (phase_of(observed) == Phase::Borrowed) {
                r.pause_ms(r.context,1);
                observed = j.control.load(std::memory_order_acquire);
                continue;
            }
#if defined(F145_HOST_TEST_HOOKS)
            host_before_cleanup_publish(j,observed);
#endif
            const auto next_phase = !cleanup_ok ? Phase::CleanupFailed :
                ((observed & kRetired) ? Phase::Acked : Phase::Closed);
            const auto next = (observed & ~kPhaseMask) | kClose |
                              static_cast<std::uint32_t>(next_phase);
            // A concurrent retire/close makes this fail and refreshes observed. Never publish
            // flags from an earlier read. Cleanup failure retains retirement but never clean Ack.
            if (!j.control.compare_exchange_strong(observed,next,std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) continue;
            if (next_phase == Phase::Acked || next_phase == Phase::CleanupFailed)
                return; // Successful CAS is the final Job access; only local POD cleanup follows.
            break;
        }
    }
}
} // namespace tk::ota

#include "stack_watch.hpp"
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace tk::ota {
namespace {
std::uint64_t network_now(void*) noexcept {
    return static_cast<std::uint64_t>(esp_timer_get_time()) / 1000;
}
std::size_t network_largest(void*) noexcept {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void network_pause(void*,unsigned ms) noexcept { vTaskDelay(pdMS_TO_TICKS(ms)); }
}
void ota_network_task(void* argument) {
    tk::stack_watch_sample(tk::StackWatch::OtaNetwork);
    tk::StackWatchScope f135_task_exit(tk::StackWatch::OtaNetwork);
    try {
        const NetRuntime runtime{nullptr,network_now,network_largest,network_pause};
        run_network_worker(*static_cast<Job*>(argument),runtime);
        // Ack/CleanupFailed was the worker's last Job access. No argument/Job destructor or
        // status/owner/OTA/logging call may follow; RTOS reclaim is not inferred from Ack.
        tk::stack_watch_sample(tk::StackWatch::OtaNetwork);
        vTaskDelete(nullptr);
    } catch (...) {
        // No foreign TLS cleanup, false Ack, slot release or writer authority on this path.
        tk::stack_watch_sample(tk::StackWatch::OtaNetwork);
        vTaskDelete(nullptr);
    }
}
}
