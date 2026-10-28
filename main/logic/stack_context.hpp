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
    Display,
    Led,
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
static_assert(kStackContextCount == 27 && kStackContextCount <= stack_sample::kCapacity);
inline constexpr const char* stack_context_name(StackContext context) noexcept {
    switch (context) {
        case StackContext::Httpd: return "httpd";
        case StackContext::Vehicle: return "vehicle";
        case StackContext::AutoPair: return "auto_pair";
        case StackContext::Mqtt: return "mqtt";
        case StackContext::Main: return "main";
        case StackContext::HeapSupervisor: return "heap_supervisor";
        case StackContext::Display: return "display";
        case StackContext::Led: return "led";
        case StackContext::OtaCheck: return "ota_check";
        case StackContext::OtaWriter: return "ota_writer";
        case StackContext::OtaGate: return "ota_gate";
        case StackContext::NetWatchdog: return "net_watchdog";
        case StackContext::Syslog: return "syslog";
        case StackContext::CaptiveDns: return "captive_dns";
        case StackContext::CaptiveHttp: return "captive_http";
        case StackContext::WifiEvent: return "wifi_event";
        case StackContext::EthEvent: return "eth_event";
        case StackContext::MqttEvent: return "mqtt_event";
        case StackContext::MqttProbeEvent: return "mqtt_probe_event";
        case StackContext::NimbleHost: return "nimble_host";
        case StackContext::BleScanTimer: return "ble_scan_timer";
        case StackContext::DiagLogCallback: return "diag_log_callback";
        case StackContext::SntpSync: return "sntp_sync";
        case StackContext::PingNet: return "ping_net";
        case StackContext::PingSyslog: return "ping_syslog";
        case StackContext::OtaNetwork: return "ota_network";
        case StackContext::OtaParser: return "ota_parser";
        default: return nullptr;
    }
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
