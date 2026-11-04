// Actual provisioning wait/close bodies against passive SDK and monotonic-clock doubles.
#include "logic/setup_portal.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "Portal CHECK line %d: %s\n", __LINE__, #condition); std::exit(42); } } while (0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_WIFI_NOT_STARTED = 11, WIFI_MODE_STA = 1;
struct NvsStorageAdapter {};
struct SetupPortal { void* ap_netif; void* server; };
struct Flag { bool load() { return false; } void store(bool) {} };
static Flag s_dns_stop, s_dns_running;
static const char* TAG = "test";
static int64_t now_us;
static bool station, ethernet, ethernet_started, wifi_stopped;
static unsigned http_calls, stop_calls, deinit_calls, mode_calls, destroy_calls, attempt_calls;
static unsigned fail_http, fail_stop, fail_deinit;
static bool terminal;
static void vTaskDelay(int ms) { now_us += int64_t(ms) * 1000; CHECK(now_us < 100000000); }
static int64_t esp_timer_get_time() { return now_us; }
static int httpd_stop(void*) { ++http_calls; if (fail_http) { --fail_http; return 1; } return ESP_OK; }
static int esp_wifi_set_mode(int) { ++mode_calls; CHECK(station); return ESP_OK; }
static int esp_wifi_stop() {
    ++stop_calls;
    if (fail_stop) { --fail_stop; return 1; }
    if (wifi_stopped) return ESP_ERR_WIFI_NOT_STARTED;
    wifi_stopped = true;
    return ESP_OK;
}
[[maybe_unused]] static int esp_wifi_deinit() {
    CHECK(wifi_stopped);
    ++deinit_calls;
    if (fail_deinit) { --fail_deinit; return 1; }
    return ESP_OK;
}
static void esp_netif_destroy_default_wifi(void*) {
    CHECK(http_calls > 0);
    CHECK(station ? mode_calls > 0 : wifi_stopped && deinit_calls > 0);
    ++destroy_calls;
}
namespace tk {
enum class StackWatch { Main };
struct StackWatchScope { explicit StackWatchScope(StackWatch) {} };
static bool net_wifi_station_started() { return station; }
[[maybe_unused]] static bool net_eth_present() { return ethernet; }
[[maybe_unused]] static bool net_eth_started() { return ethernet_started; }
static bool net_is_up() { return now_us >= 2000000; }
static void net_wifi_retry_station() { CHECK(station); ++attempt_calls; }
}
static SetupPortal portal_open(NvsStorageAdapter&, bool keep_station) {
    CHECK(keep_station == station);
    return {reinterpret_cast<void*>(1), reinterpret_cast<void*>(2)};
}
static uint32_t portal_ap_clients() { return 1; }
static void provisioning_run(NvsStorageAdapter&) { terminal = true; throw 1; }

#include "portal_cleanup.generated.hpp"

int main() {
    (void)TAG;
    NvsStorageAdapter config;
    for (int fault = 0; fault < 5; ++fault) {
        now_us = 0;
        station = fault == 4;
        ethernet = ethernet_started = true;
        wifi_stopped = terminal = false;
        http_calls = stop_calls = deinit_calls = mode_calls = destroy_calls = attempt_calls = 0;
        fail_http = fault == 1;
        fail_stop = fault == 2;
        fail_deinit = fault == 3;
        provisioning_run_with_retry(config);
        CHECK(!terminal && destroy_calls == 1 && attempt_calls == 0);
        CHECK(station ? stop_calls == 0 && deinit_calls == 0 && mode_calls == 1 :
              stop_calls > 0 && deinit_calls > 0 && mode_calls == 0);
        CHECK(fault >= 1 && fault <= 3 ? now_us >= 32000000 : now_us < 3000000);
        CHECK(http_calls == (fault == 1 ? 2u : 1u));
        CHECK(stop_calls == (station ? 0u : fault == 2 || fault == 3 ? 2u : 1u));
    }
    for (bool detected : {false, true}) {
        station = ethernet_started = terminal = false;
        ethernet = detected;  // A detected chip with a failed driver cannot recover a lease.
        try { provisioning_run_with_retry(config); CHECK(false); } catch (int) {}
        CHECK(terminal);
    }
    std::puts("PASS actual portal recovery and partial-cleanup retries");
}
