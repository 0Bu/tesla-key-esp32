// Actual stack watcher shell/codec tests; mandatory CTest, fast, fallback and sanitizer consumers.
#include "../main/stack_watch.cpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <cstring>
#include <string>

namespace sc = tk::stack_sample;
static unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { \
    std::fprintf(stderr, "stack probe %u: %s\n", unsigned(__LINE__), #value); std::exit(1); } } while (false)
static thread_local UBaseType_t own_free_bytes = 0;
static std::atomic<unsigned> hwm_queries{0};
static std::atomic<bool> foreign_task_query{false};
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    if (task != nullptr) foreign_task_query.store(true);
    hwm_queries.fetch_add(1);
    return own_free_bytes;
}

static void bounds() {
    CHECK(!sc::decode(0).sampled && !sc::decode(64).sampled && !sc::decode(255).sampled);
    CHECK(sc::encode(0) == 1 && sc::encode(255) == 1 && sc::encode(256) == 2);
    CHECK(sc::encode(15871) == 62 && sc::encode(15872) == 63);
    CHECK(sc::encode(UINT32_MAX) == 63 && sc::decode(63).saturated);
    CHECK(sc::decode(63).lower_bound_bytes == 15872);
    for (uint32_t raw = 0; raw <= 65535; ++raw) {
        const auto value = sc::decode(sc::encode(raw));
        CHECK(value.sampled && value.lower_bound_bytes <= raw);
        CHECK(value.saturated || raw - value.lower_bound_bytes < 256);
        CHECK(!value.saturated || value.lower_bound_bytes == 15872);
    }
    for (unsigned lane = 0; lane != 5; ++lane) {
        for (uint32_t seed : {UINT32_C(0), UINT32_C(0xffffffff), UINT32_C(0x2468ace0)}) {
            for (uint32_t raw : {0u, 255u, 256u, 4095u, 4096u, 15871u, 15872u, UINT32_MAX}) {
                const auto next = sc::merge(seed, lane, raw);
                CHECK((next & ~(UINT32_C(63) << (lane * 6))) == (seed & ~(UINT32_C(63) << (lane * 6))));
                CHECK(sc::merge(next, lane, raw) == next);
                const auto old = uint8_t((seed >> (lane * 6)) & 63);
                const auto value = sc::decode(uint8_t((next >> (lane * 6)) & 63));
                CHECK(value.sampled && value.lower_bound_bytes <= raw);
                CHECK(old == 0 || value.lower_bound_bytes <= sc::decode(old).lower_bound_bytes);
            }
        }
    }
    CHECK(sc::merge(0x12345678, 5, 0) == 0x12345678);
    sc::Snapshot reserved{}; reserved.fill(UINT32_C(0xc0000000));
    CHECK(!sc::any_sample(reserved) && !sc::read(reserved, sc::kCapacity).sampled);
}

// A deterministic passive atomic double updates expected exactly as compare_exchange_strong
// does after a competing write. The same publish_minimum program serves it and std::atomic.
struct CompetingWord {
    uint32_t value{UINT32_C(0xc0000000)};
    unsigned other_lane{0}, calls{0};
    uint32_t load(std::memory_order) const noexcept { return value; }
    bool compare_exchange_strong(uint32_t& expected, uint32_t desired,
                                 std::memory_order, std::memory_order) noexcept {
        if (calls++ == 0) {
            value = sc::merge(value, other_lane, 0);
            expected = value;
            return false;
        }
        if (expected != value) { expected = value; return false; }
        value = desired;
        return true;
    }
};
static void deterministic_competing_updates() {
    for (unsigned own = 0; own != 5; ++own) for (unsigned other = 0; other != 5; ++other) {
        CompetingWord word; word.other_lane = other;
        sc::publish_minimum(word, own, 1024);
        CHECK((word.value & UINT32_C(0xc0000000)) == UINT32_C(0xc0000000));
        CHECK(((word.value >> (other * 6)) & 63) == 1); // competing measured0 survives
        CHECK(((word.value >> (own * 6)) & 63) == (own == other ? 1u : 5u));
        CHECK(word.calls == (own == other ? 1u : 2u));
    }
}

static void actual_shell_and_threads() {
    static constexpr const char* names[] = {
        "httpd", "vehicle", "auto_pair", "mqtt", "main", "heap_supervisor", "display", "led",
        "ota_check", "ota_writer", "ota_gate", "net_watchdog", "syslog", "captive_dns",
        "captive_http", "wifi_event", "eth_event", "mqtt_event", "mqtt_probe_event",
        "nimble_host", "ble_scan_timer", "diag_log_callback", "sntp_sync", "ping_net", "ping_syslog", "ota_network", "ota_parser"};
    for (unsigned slot = 0; slot != tk::kStackContextCount; ++slot) {
        const auto context = static_cast<tk::StackWatch>(slot);
        CHECK(!tk::stack_watch_min_free_bytes(context).has_value());
        CHECK(std::strcmp(tk::stack_context_name(context), names[slot]) == 0);
        char key[tk::kStackMetricKeyBytes]{};
        CHECK(tk::stack_context_metric_key(context, key));
        const std::string expected = std::string(names[slot]) + "_stack_min_free_bytes";
        CHECK(expected == key);
    }
    const auto before_invalid = hwm_queries.load();
    tk::stack_watch_sample(tk::StackWatch::Count);
    tk::stack_watch_sample(static_cast<tk::StackWatch>(255));
    CHECK(hwm_queries.load() == before_invalid);
    CHECK(!tk::stack_watch_min_free_bytes(static_cast<tk::StackWatch>(255)).has_value());
    char unchanged[tk::kStackMetricKeyBytes] = "keep";
    CHECK(!tk::stack_context_metric_key(tk::StackWatch::Count, unchanged));
    CHECK(std::strcmp(unchanged, "keep") == 0);
    for (uint32_t raw : {1000u, 1200u, 600u, 900u, 0u, 2000u}) {
        own_free_bytes = raw; tk::stack_watch_sample(tk::StackWatch::Httpd);
        const uint32_t expected = raw == 1000 || raw == 1200 ? 768 : raw == 600 || raw == 900 ? 512 : 0;
        CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::Httpd) == expected);
    }
    own_free_bytes = 511;
    { tk::StackWatchScope scope(tk::StackWatch::AutoPair); }
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::AutoPair) == 256);
    own_free_bytes = 200;
    try { tk::StackWatchScope scope(tk::StackWatch::Main); throw 42; } catch (...) {}
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::Main) == 0);
    own_free_bytes = 4096; tk::stack_watch_sample(tk::StackWatch::Mqtt);
    const auto frozen = tk::stack_watch_snapshot();
    own_free_bytes = 256; tk::stack_watch_sample(tk::StackWatch::Mqtt);
    CHECK(sc::read(frozen, 3).lower_bound_bytes == 4096);
    CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::Mqtt) == 256);
    const auto prior = tk::stack_watch_snapshot();
    std::atomic<bool> start{false}; std::array<std::thread, tk::kStackContextCount> threads;
    for (unsigned slot = 0; slot != tk::kStackContextCount; ++slot) threads[slot] = std::thread([slot, &start] {
        while (!start.load()) std::this_thread::yield();
        const auto context = static_cast<tk::StackWatch>(slot);
        for (unsigned n = 0; n != 2000; ++n) {
            own_free_bytes = (slot + 1) * 256 - 1 + (n % 7) * 256;
            tk::stack_watch_sample(context);
        }
    });
    start.store(true);
    for (auto& thread : threads) thread.join();
    for (unsigned slot = 0; slot != tk::kStackContextCount; ++slot) {
        const auto old = sc::read(prior, slot);
        const uint32_t expected = old.sampled ? std::min(old.lower_bound_bytes, slot * 256) : slot * 256;
        CHECK(tk::stack_watch_min_free_bytes(static_cast<tk::StackWatch>(slot)) == expected);
    }
    CHECK(!foreign_task_query.load());
}
int main() {
    bounds(); deterministic_competing_updates(); actual_shell_and_threads();
    std::printf("stack sample actual shell: %u checks OK\n", checks);
}
