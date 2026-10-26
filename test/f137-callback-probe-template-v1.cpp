#include <atomic>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "runtime_fault_log.hpp"
#include "stack_watch.hpp"
#include "freertos/task.h"
#include "logic/ble_readiness.hpp"
#include "logic/ble_chunk.hpp"

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    std::exit(42); } } while (false)

namespace fixture {
std::int64_t now_us;
bool throw_reset_log, throw_fault_sink, throw_sync, throw_timer, throw_connected;
bool lock_available = true;
unsigned sync_calls, timer_calls, connected_calls;
std::vector<std::string> warnings;

void log(int level, const char*, const char* format, ...) {
    if (level < 2) return; // Actual configured INFO default: DEBUG is invisible.
    char message[192];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    warnings.emplace_back(message);
    if (throw_reset_log && std::strstr(format, "NimBLE host reset"))
        throw std::runtime_error("passive normal diagnostic failure");
    if (throw_fault_sink && std::strstr(format, "callback failed"))
        throw std::runtime_error("passive bounded sink failure");
}
}

#define ESP_LOGW(tag, ...) fixture::log(2, tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) fixture::log(0, tag, __VA_ARGS__)
static const char* TAG = "ble_client";
static std::int64_t esp_timer_get_time() { return fixture::now_us; }
static constexpr std::uint16_t BLE_HS_CONN_HANDLE_NONE = 0xffff;

namespace tk {
class SemGuard {
public:
    SemGuard(void*, unsigned wait) { CHECK(wait == 0); }
    explicit operator bool() const { return fixture::lock_available; }
};
}

class BleClient {
public:
    void on_sync() {
        ++fixture::sync_calls;
        if (fixture::throw_sync) throw std::bad_alloc();
    }
    void on_scan_timeout() {
        ++fixture::timer_calls;
        if (fixture::throw_timer) throw 1;
    }
    void on_reset();
    void report_callback_fault(bool, const char*) noexcept;

    std::atomic<std::uint32_t> host_reset_count_{0}, connection_generation_{10};
    std::atomic<std::uint32_t> ready_generation_{10};
    std::atomic<bool> want_connect_{true}, disconnecting_{false};
    std::atomic<bool> host_synced_{true}, scanning_{true}, connecting_{true};
    std::atomic<bool> conn_rssi_valid_{true};
    std::atomic<std::uint16_t> conn_handle_{7}, write_handle_{9};
    std::atomic<std::size_t> write_payload_size_{244};
    void* intent_mutex_{nullptr};
    using Connected = bool (*)(void*, bool, std::uint16_t, std::uint32_t);
    Connected on_connected_{[](void*, bool up, std::uint16_t handle, std::uint32_t generation) {
        ++fixture::connected_calls;
        CHECK(!up && handle == BLE_HS_CONN_HANDLE_NONE && generation % 2 == 0);
        if (fixture::throw_connected) throw std::bad_alloc();
        return true;
    }};
    void* connected_context_{nullptr};
    tk::IterationFaultLog host_callback_faults_{}, timer_callback_faults_{};
};
static BleClient* g_instance;

extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task){CHECK(task==nullptr);return 4096u;}

// ACTUAL_COMPLETE_BODIES

static void reset_fixture(BleClient& client) {
    g_instance = &client;
    fixture::now_us = 0;
    fixture::throw_reset_log = fixture::throw_fault_sink = false;
    fixture::throw_sync = fixture::throw_timer = fixture::throw_connected = false;
    fixture::lock_available = true;
    fixture::sync_calls = fixture::timer_calls = fixture::connected_calls = 0;
    fixture::warnings.clear();
}

static void check_published(const BleClient& client) {
    CHECK(client.host_reset_count_ == 1);
    CHECK(!client.host_synced_ && !client.scanning_ && !client.disconnecting_);
    CHECK(client.conn_handle_ == BLE_HS_CONN_HANDLE_NONE && client.write_handle_ == 0);
    CHECK(client.ready_generation_ == tk::ble::kNoReadyGeneration);
    CHECK(client.write_payload_size_ == tk::kBleDefaultWritePayload);
    CHECK(!client.conn_rssi_valid_ && client.connection_generation_ == 12);
    CHECK(fixture::connected_calls == 1);
}

int main() {
    // A throwing normal reset diagnostic cannot skip actual sticky/host-down publication.
    { BleClient c; reset_fixture(c); fixture::throw_reset_log = true;
      on_reset_cb(6); check_published(c); CHECK(c.host_callback_faults_.emitted); }
    { BleClient c; reset_fixture(c); fixture::throw_connected = true;
      on_reset_cb(6); check_published(c); CHECK(c.host_callback_faults_.emitted); }
    { BleClient c; reset_fixture(c); fixture::lock_available = false;
      on_reset_cb(6); check_published(c); CHECK(!c.want_connect_); }
    // Shared host suppression survives successful calls; the timer owns a separate quota.
    { BleClient c; reset_fixture(c); fixture::throw_sync = fixture::throw_timer = true;
      on_sync_cb(); scan_timeout_cb(&c); CHECK(fixture::warnings.size() == 2);
      fixture::now_us = 1000000; on_sync_cb(); scan_timeout_cb(&c);
      CHECK(fixture::warnings.size() == 2);
      CHECK(c.host_callback_faults_.suppressed == 1 && c.timer_callback_faults_.suppressed == 1);
      fixture::throw_sync = fixture::throw_timer = false; on_sync_cb(); scan_timeout_cb(&c);
      fixture::throw_sync = fixture::throw_timer = true; fixture::now_us = 29999000;
      on_sync_cb(); scan_timeout_cb(&c); CHECK(fixture::warnings.size() == 2);
      fixture::now_us = 30000000; on_sync_cb(); scan_timeout_cb(&c);
      CHECK(fixture::warnings.size() == 4);
      CHECK(fixture::warnings[2].find("suppressed=2") != std::string::npos);
      CHECK(fixture::warnings[3].find("suppressed=2") != std::string::npos);
      c.host_callback_faults_.suppressed = UINT32_MAX;
      fixture::now_us = 31000000; on_sync_cb(); CHECK(c.host_callback_faults_.suppressed == UINT32_MAX);
      fixture::now_us = 60000000; on_sync_cb();
      CHECK(fixture::warnings.back().find("suppressed=4294967295") != std::string::npos); }
    // A failing sink consumes the interval and never escapes a callback or gains a replay.
    { BleClient c; reset_fixture(c); fixture::throw_sync = fixture::throw_fault_sink = true;
      on_sync_cb(); CHECK(fixture::warnings.size() == 1 && fixture::sync_calls == 1);
      fixture::now_us = 1000000; on_sync_cb();
      CHECK(fixture::warnings.size() == 1 && fixture::sync_calls == 2);
      fixture::throw_fault_sink = false; fixture::now_us = 30000000; on_sync_cb();
      CHECK(fixture::warnings.size() == 2 && fixture::sync_calls == 3); }
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::NimbleHost) == 4096u);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::BleScanTimer) == 4096u);
    std::printf("PASS %u actual callback/reset checks; SDK/RTOS registration and semaphore facades only\n", checks);
}
