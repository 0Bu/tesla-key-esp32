#pragma once
#include "stack_sample_codec.hpp"
#include <cstdint>

// Pure identifiers for cached current-task samples. Callback labels identify entry contexts;
// they do not assert a dedicated SDK task. Full owner/dispatch mapping lives in Architecture.
namespace tk {
enum class StackContext : std::uint8_t {
    Httpd,
    Vehicle,
    AutoPair,
    Mqtt,
    Main,
    HeapSupervisor,
    OtaCheck,
    OtaWriter,
    OtaGate,
    NetWatchdog,
    Syslog,
    CaptiveDns,
    CaptiveHttp,
    WifiEvent,
    EthEvent,
    MqttEvent,
    MqttProbeEvent,
    NimbleHost,
    BleScanTimer,
    DiagLogCallback,
    SntpSync,
    PingNet,
    PingSyslog,
    OtaNetwork,
    OtaParser,
    Count,
};
inline constexpr unsigned kStackContextCount = static_cast<unsigned>(StackContext::Count);
static_assert(kStackContextCount == 25 && kStackContextCount <= stack_sample::kCapacity);
// Contiguous enum values index one flash-resident table; no automatic pointer array or switch bodies.
inline constexpr const char* kStackContextNames[] = {
    "httpd", "vehicle", "auto_pair", "mqtt", "main", "heap_supervisor",
    "ota_check", "ota_writer", "ota_gate", "net_watchdog", "syslog", "captive_dns", "captive_http",
    "wifi_event", "eth_event", "mqtt_event", "mqtt_probe_event", "nimble_host", "ble_scan_timer",
    "diag_log_callback", "sntp_sync", "ping_net", "ping_syslog", "ota_network", "ota_parser",
};
static_assert(sizeof(kStackContextNames) / sizeof(kStackContextNames[0]) == kStackContextCount);
inline constexpr const char* stack_context_name(StackContext context) noexcept {
    const auto index = static_cast<unsigned>(context);
    return index < kStackContextCount ? kStackContextNames[index] : nullptr;
}
// Preserves the four existing MQTT field names without heap allocation. All registered names
// plus suffix and NUL fit40B; refusal leaves the destination unchanged.
inline constexpr unsigned kStackMetricKeyBytes = 40;
inline bool stack_context_metric_key(StackContext context,
                                     char (&out)[kStackMetricKeyBytes]) noexcept {
    const char* name = stack_context_name(context);
    if (!name) return false;
    unsigned size = 0;
    while (name[size] != '\0') ++size;
    constexpr char suffix[] = "_stack_min_free_bytes";
    if (size + sizeof(suffix) > sizeof(out)) return false;
    for (unsigned i = 0; i < size; ++i) out[i] = name[i];
    for (unsigned i = 0; i < sizeof(suffix); ++i) out[size + i] = suffix[i];
    return true;
}
}  // namespace tk
