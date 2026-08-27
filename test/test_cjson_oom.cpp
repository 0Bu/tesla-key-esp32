#include "json_builder.hpp"
#include "json_http_reply.hpp"
#include "logic/json_syntax.hpp"
#include "logic/status_model.hpp"
#include "mcp_json_payloads.hpp"
#include "ota_manifest.hpp"
#include "status_json_emitter.hpp"

#include <cJSON.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

size_t checks = 0;
size_t allocation_attempt = 0;
size_t fail_at = 0;
size_t live_allocations = 0;
size_t live_bytes = 0;
size_t peak_live_bytes = 0;

struct alignas(std::max_align_t) AllocationHeader {
    size_t size;
};

#define CHECK(condition)                                                         \
    do {                                                                         \
        ++checks;                                                                \
        if (!(condition)) {                                                      \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": "         \
                      << #condition << '\n';                                     \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

void* tracked_malloc(size_t size) {
    ++allocation_attempt;
    if (fail_at != 0 && allocation_attempt == fail_at) return nullptr;
    AllocationHeader* header = static_cast<AllocationHeader*>(
        std::malloc(sizeof(AllocationHeader) + size));
    if (!header) return nullptr;
    header->size = size;
    ++live_allocations;
    live_bytes += size;
    if (live_bytes > peak_live_bytes) peak_live_bytes = live_bytes;
    return header + 1;
}

void tracked_free(void* value) {
    if (!value) return;
    CHECK(live_allocations > 0);
    AllocationHeader* header = static_cast<AllocationHeader*>(value) - 1;
    CHECK(live_bytes >= header->size);
    live_bytes -= header->size;
    --live_allocations;
    std::free(header);
}

void reset_allocator(size_t failure) {
    CHECK(live_allocations == 0);
    CHECK(live_bytes == 0);
    allocation_attempt = 0;
    fail_at = failure;
    peak_live_bytes = 0;
}

struct HttpReplySpy {
    bool json_type{false};
    bool status_set{false};
    int status{200};
    int status_at_send{-1};
    size_t send_calls{0};
    char body[4096]{};

    void set_json_type() noexcept { json_type = true; }
    void set_status(int value) noexcept {
        status_set = true;
        status = value;
    }
    int send(const char* value) noexcept {
        ++send_calls;
        status_at_send = status_set ? status : 200;
        if (!value) {
            body[0] = '\0';
            return -1;
        }
        std::strncpy(body, value, sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';
        return 0;
    }
};

tk::JsonOwner build_status_fixture() {
    tk::status::Inputs inputs;
    inputs.ip = "192.0.2.1";
    inputs.vin = "5YJ00000000000000";
    inputs.version = "0.0.0-test";
    inputs.key_present = true;
    inputs.key_fingerprint = "AA:BB:CC:DD:EE:FF";
    inputs.key_created = 1750000000;
    inputs.paired = true;
    inputs.paired_at = 1750000100;
    inputs.reauth = true;
    inputs.wifi_connected = true;
    inputs.wifi_ssid = "fixture";
    inputs.wifi_rssi = -61;
    inputs.wifi_std = "Wi-Fi 6";
    inputs.wifi_rolled_back = true;
    inputs.eth_link = true;
    inputs.eth_speed_mbps = 100;
    inputs.eth_full_duplex = true;
    inputs.mqtt_configured = true;
    inputs.mqtt_connected = true;
    inputs.mqtt_tls = true;
    inputs.mqtt_broker = "mqtts://broker.example.invalid:8883";
    inputs.mqtt_error = "last bounded broker error";
    inputs.syslog_configured = true;
    inputs.syslog_resolved = true;
    inputs.syslog_reachable = true;
    inputs.syslog_host = "syslog.example.invalid";
    inputs.syslog_port = 514;
    inputs.syslog_error = "last bounded syslog error";
    inputs.ble_connected = true;
    inputs.ble_scanning = true;
    inputs.have_ble_rssi = true;
    inputs.ble_rssi = -62;
    inputs.ble_addr = "00:11:22:33:44:55";
    inputs.ble_phase = "connecting";
    inputs.ble_phase_s = 19;
    inputs.link = tk::LinkState::Awake;
    inputs.vcsec_sleep = "AWAKE";
    inputs.climate.valid = true;
    inputs.climate.has_inside = inputs.climate.has_outside = inputs.climate.has_setpoint = true;
    inputs.climate.inside_temp = 21.5;
    inputs.climate.outside_temp = 8.5;
    inputs.climate.driver_setpoint = 20.0;
    inputs.climate.has_climate_on = inputs.climate.has_preconditioning = true;
    inputs.climate.is_climate_on = true;
    inputs.climate.is_preconditioning = false;
    inputs.climate.has_cop = inputs.climate.has_cop_cooling = true;
    inputs.climate.cop = "On";
    inputs.climate.cop_cooling = false;
    inputs.climate.has_cop_temp = inputs.climate.has_cop_reason = true;
    inputs.climate.cop_temp = "Medium";
    inputs.climate.cop_reason = "available";
    inputs.climate.has_front_defrost = inputs.climate.has_rear_defrost = true;
    inputs.climate.front_defrost = inputs.climate.rear_defrost = false;
    inputs.climate.has_defrost_mode = true;
    inputs.climate.defrost_mode = "Off";
    inputs.drive.valid = true;
    inputs.drive.shift_state = "P";
    inputs.drive.has_odometer = true;
    inputs.drive.odometer_km = 12345.5;
    inputs.tires.valid = true;
    inputs.tires.has_fl = inputs.tires.has_fr = inputs.tires.has_rl = inputs.tires.has_rr = true;
    inputs.tires.fl = 2.7;
    inputs.tires.fr = 2.8;
    inputs.tires.rl = 2.9;
    inputs.tires.rr = 3.0;
    inputs.closures.valid = true;
    inputs.closures.has_locked = true;
    inputs.closures.locked = true;
    inputs.closures.has_user_present = true;
    inputs.closures.user_present = true;
    inputs.charge.valid = true;
    inputs.charge.has_battery_level = true;
    inputs.charge.battery_level = 63;
    inputs.charge.charging_state = "Charging";
    inputs.charge.has_charge_limit_soc = inputs.charge.has_charger_power = true;
    inputs.charge.charge_limit_soc = 80;
    inputs.charge.charger_power = 11;
    inputs.charge.has_charging_amps = inputs.charge.has_actual_current = true;
    inputs.charge.charging_amps = 16;
    inputs.charge.charger_actual_current = 15;
    inputs.charge.has_voltage = inputs.charge.has_charger_phases = true;
    inputs.charge.charger_voltage = 231;
    inputs.charge.charger_phases = 3;
    inputs.have_last_seen = true;
    inputs.last_seen_s = 7;
    inputs.last_reboot = "heap:2";
    inputs.board_mac = "02:00:00:00:00:01";
    inputs.free_heap = 16384;
    inputs.min_free_heap = 12288;
    inputs.largest_block = 8192;
    inputs.uptime_s = 123456;
    inputs.wifi_reconnects = 4;
    inputs.reset_reason = "power_on";
    inputs.httpd_stack_min_free_bytes = 4096;
    inputs.vehicle_stack_min_free_bytes = 3072;
    inputs.auto_pair_stack_min_free_bytes = 2048;
    inputs.mqtt_stack_min_free_bytes = 1024;
    inputs.have_crash = true;
    inputs.crash_reason = "panic";
    inputs.crash_reason_code = 3;
    inputs.crash_fault = true;
    inputs.crash_coredump = true;
    inputs.crash_corrupted = true;
    inputs.crash_task = "vehicle";
    inputs.crash_pc = 0x40001234;
    inputs.crash_backtrace = {0x40001234, 0x40005678, 0x40009abc, 0x4000def0};
    inputs.crash_elf_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    tk::JsonBuilder json;
    tk::StatusJsonEmitter emitter(json);
    tk::status::emit_status(inputs, emitter);
    return emitter.finish();
}

void test_status_emitter_structure_guards() {
    reset_allocator(0);
    {
        tk::JsonBuilder json;
        tk::StatusJsonEmitter emitter(json);
        for (size_t i = 1; i < tk::StatusJsonEmitter::kStackCapacity; ++i) {
            emitter.obj_begin("nested");
        }
        for (size_t i = 1; i < tk::StatusJsonEmitter::kStackCapacity; ++i) {
            emitter.obj_end();
        }
        tk::JsonOwner root = emitter.finish();
        CHECK(root != nullptr);
    }
    CHECK(live_allocations == 0);

    reset_allocator(0);
    {
        tk::JsonBuilder json;
        tk::StatusJsonEmitter emitter(json);
        for (size_t i = 0; i < tk::StatusJsonEmitter::kStackCapacity + 1; ++i) {
            emitter.obj_begin("overflow");
        }
        CHECK(emitter.finish() == nullptr);
    }
    CHECK(live_allocations == 0);

    reset_allocator(0);
    {
        tk::JsonBuilder json;
        tk::StatusJsonEmitter emitter(json);
        emitter.obj_end();
        CHECK(emitter.finish() == nullptr);
    }
    CHECK(live_allocations == 0);

    reset_allocator(0);
    {
        tk::JsonBuilder json;
        tk::StatusJsonEmitter emitter(json);
        emitter.arr_begin("open");
        CHECK(emitter.finish() == nullptr);
    }
    CHECK(live_allocations == 0);
}

tk::JsonOwner build_rest_fixture() {
    tk::JsonBuilder json;
    cJSON* outer = json.object(json.root(), "response");
    json.boolean(outer, "result", true);
    json.string(outer, "vin", "5YJ00000000000000");
    cJSON* response = json.object(outer, "response");
    cJSON* charge = json.object(response, "charge_state");
    json.string(charge, "charging_state", "Charging");
    json.number(charge, "battery_level", 63);
    json.number(charge, "charge_amps", 16);
    return json.finish();
}

tk::JsonOwner build_mcp_result_fixture() {
    tk::mcp_json::RpcId id;
    id.kind = tk::mcp_json::RpcIdKind::Number;
    id.number = 7;
    return tk::mcp_json::build_result_envelope(
        id, tk::mcp_json::build_tool_result("command succeeded", false));
}

tk::JsonOwner build_mcp_max_safe_id_fixture() {
    tk::mcp_json::RpcId id;
    id.kind = tk::mcp_json::RpcIdKind::Number;
    id.number = tk::kJsonSafeIntegerMax;
    return tk::mcp_json::build_error_envelope(id, -32600, "fixture");
}

tk::JsonOwner build_mcp_error_fixture() {
    tk::mcp_json::RpcId id;
    id.kind = tk::mcp_json::RpcIdKind::String;
    std::strcpy(id.string, "request-1");
    return tk::mcp_json::build_error_envelope(id, -32602, "invalid params");
}

tk::JsonOwner build_tools_list_fixture() {
    tk::mcp_json::RpcId id;
    id.kind = tk::mcp_json::RpcIdKind::String;
    std::strcpy(id.string, "tools-list-gate");
    return tk::mcp_json::build_result_envelope(
        id, tk::mcp_json::build_tools_list_result());
}

tk::JsonOwner build_vehicle_state_fixture() {
    tk::mcp_json::VehicleStatePayload state;
    state.vin = "5YJ00000000000000";
    state.paired = true;
    state.link = "awake";
    state.has_last_seen = true;
    state.last_seen_s = 5;
    state.has_soc = true;
    state.soc = 63;
    state.charging_state = "Charging";
    state.has_charge_limit = true;
    state.charge_limit = 80;
    state.has_charge_amps = true;
    state.charge_amps = 16;
    state.has_charger_power = true;
    state.charger_power_kw = 11;
    tk::mcp_json::RpcId id;
    id.kind = tk::mcp_json::RpcIdKind::Number;
    id.number = 8;
    return tk::mcp_json::build_result_envelope(
        id, tk::mcp_json::build_vehicle_state_result(state));
}

template <typename Build>
void exhaust_every_allocation(const char* name, Build build) {
    reset_allocator(0);
    size_t allocation_count = 0;
    {
        tk::JsonOwner root = build();
        CHECK(root != nullptr);
        allocation_count = allocation_attempt;
        CHECK(allocation_count > 0);
    }
    CHECK(live_allocations == 0);

    for (size_t nth = 1; nth <= allocation_count; ++nth) {
        reset_allocator(nth);
        {
            tk::JsonOwner root = build();
            CHECK(root == nullptr);
        }
        CHECK(live_allocations == 0);
    }
    std::cout << "  " << name << ": " << allocation_count << " allocation stages\n";
}

template <typename Build>
void exhaust_build_and_print(const char* name, Build build, const char* oom_body) {
    reset_allocator(0);
    size_t build_allocations = 0;
    size_t total_allocations = 0;
    {
        HttpReplySpy transport;
        tk::JsonOwner root = build();
        CHECK(root != nullptr);
        build_allocations = allocation_attempt;
        CHECK(tk::json_http_reply(transport, std::move(root), 200, oom_body) == 0);
        total_allocations = allocation_attempt;
        CHECK(transport.json_type);
        CHECK(transport.send_calls == 1);
        CHECK(transport.status_at_send == 200);
        CHECK(std::strcmp(transport.body, oom_body) != 0);
        CHECK(total_allocations > build_allocations);  // Print stage is part of the matrix.
    }
    CHECK(live_allocations == 0);

    for (size_t nth = 1; nth <= total_allocations; ++nth) {
        reset_allocator(nth);
        {
            HttpReplySpy transport;
            CHECK(tk::json_http_reply(transport, build(), 200, oom_body) == 0);
            // Neither a partially built body nor a default-200 can escape: the shared production
            // seam performs exactly one fallback send, after setting 503.
            CHECK(transport.json_type);
            CHECK(transport.send_calls == 1);
            CHECK(transport.status_at_send == 503);
            CHECK(std::strcmp(transport.body, oom_body) == 0);
        }
        CHECK(live_allocations == 0);
    }

    reset_allocator(0);
    {
        HttpReplySpy transport;
        CHECK(tk::json_http_reply(transport, build(), 409, oom_body) == 0);
        CHECK(transport.send_calls == 1);
        CHECK(transport.status_at_send == 409);
        CHECK(std::strcmp(transport.body, oom_body) != 0);
    }
    CHECK(live_allocations == 0);
    std::cout << "  " << name << ": " << total_allocations
              << " build+print allocation stages\n";
}

size_t tools_list_released_bytes(const char* body,
                                 tk::mcp_json::RpcIdStatus expected,
                                 tk::mcp_json::RpcId* captured = nullptr) {
    reset_allocator(0);
    const tk::JsonRawNumberId raw_numeric_id =
        tk::json_top_level_numeric_id(body, std::strlen(body));
    size_t peak = 0;
    {
        tk::JsonOwner msg(cJSON_Parse(body));
        CHECK(msg != nullptr);
        peak = peak_live_bytes;
        tk::mcp_json::RpcId id;
        const auto status = tk::mcp_json::capture_id(
            cJSON_GetObjectItemCaseSensitive(msg.get(), "id"), raw_numeric_id, id);
        CHECK(status == expected);
        const cJSON* method = cJSON_GetObjectItemCaseSensitive(msg.get(), "method");
        CHECK(cJSON_IsString(method));
        CHECK(std::strcmp(method->valuestring, "tools/list") == 0);
        msg.reset();
        CHECK(live_allocations == 0);
        CHECK(live_bytes == 0);
        if (captured) *captured = id;
    }
    CHECK(live_allocations == 0);
    CHECK(live_bytes == 0);
    return peak;
}

void test_max_tools_list_input_lifetime() {
    static constexpr char minimal[] =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}";
    char maximal[2049]{};
    static constexpr char prefix[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"12345678901234567890123456789012"
        "12345678901234567890123456789012\",\"method\":\"tools/list\",\"padding\":\"";
    static constexpr char suffix[] = "\"}";
    const size_t prefix_len = std::strlen(prefix);
    const size_t suffix_len = std::strlen(suffix);
    CHECK(prefix_len + suffix_len < 2048);
    std::memcpy(maximal, prefix, prefix_len);
    std::memset(maximal + prefix_len, 'x', 2048 - prefix_len - suffix_len);
    std::memcpy(maximal + 2048 - suffix_len, suffix, suffix_len);
    maximal[2048] = '\0';
    CHECK(std::strlen(maximal) == 2048);

    tk::mcp_json::RpcId captured;
    const size_t small_peak = tools_list_released_bytes(
        minimal, tk::mcp_json::RpcIdStatus::Valid);
    const size_t large_peak = tools_list_released_bytes(
        maximal, tk::mcp_json::RpcIdStatus::Valid, &captured);
    CHECK(large_peak > small_peak);
    CHECK(captured.kind == tk::mcp_json::RpcIdKind::String);
    CHECK(std::strlen(captured.string) == tk::mcp_json::kMaxStringIdBytes);

    char oversized[2049]{};
    static constexpr char oversized_prefix[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"";
    static constexpr char oversized_suffix[] = "\",\"method\":\"tools/list\"}";
    const size_t op = std::strlen(oversized_prefix);
    const size_t os = std::strlen(oversized_suffix);
    std::memcpy(oversized, oversized_prefix, op);
    std::memset(oversized + op, 'y', 2048 - op - os);
    std::memcpy(oversized + 2048 - os, oversized_suffix, os);
    oversized[2048] = '\0';
    CHECK(tools_list_released_bytes(oversized, tk::mcp_json::RpcIdStatus::Invalid) > 0);
    CHECK(tools_list_released_bytes(
              "{\"jsonrpc\":\"2.0\",\"id\":{\"nested\":[1,2,3]},"
              "\"method\":\"tools/list\"}",
              tk::mcp_json::RpcIdStatus::Invalid) > 0);
    CHECK(tools_list_released_bytes(
              "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"tools/list\"}",
              tk::mcp_json::RpcIdStatus::Invalid) > 0);
    std::cout << "  MCP max-body tools/list: " << large_peak
              << " peak input bytes reduced to 0 before response build\n";
}

tk::mcp_json::RpcRequestEnvelope inspect_request(const char* body) {
    const tk::JsonRawNumberId raw_numeric_id =
        tk::json_top_level_numeric_id(body, std::strlen(body));
    tk::mcp_json::RpcRequestEnvelope result;
    {
        tk::JsonOwner request(cJSON_Parse(body));
        CHECK(request != nullptr);
        result = tk::mcp_json::inspect_request_envelope(request.get(), raw_numeric_id);
    }
    CHECK(live_allocations == 0);
    CHECK(live_bytes == 0);
    return result;
}

void check_numeric_id_echo(const char* request_body, int64_t expected,
                           const char* expected_reply) {
    const auto request = inspect_request(request_body);
    CHECK(request.status == tk::mcp_json::RpcRequestStatus::Valid);
    CHECK(request.id_status == tk::mcp_json::RpcIdStatus::Valid);
    CHECK(request.id.kind == tk::mcp_json::RpcIdKind::Number);
    CHECK(request.id.number == expected);
    {
        tk::JsonOwner reply =
            tk::mcp_json::build_error_envelope(request.id, -32600, "fixture");
        CHECK(reply != nullptr);
        tk::JsonPrintOwner printed(cJSON_PrintUnformatted(reply.get()));
        CHECK(printed != nullptr);
        CHECK(std::strcmp(printed.get(), expected_reply) == 0);
        tk::JsonOwner echoed(cJSON_Parse(printed.get()));
        CHECK(echoed != nullptr);
        const cJSON* echoed_id = cJSON_GetObjectItemCaseSensitive(echoed.get(), "id");
        CHECK(cJSON_IsNumber(echoed_id));
        CHECK(echoed_id->valuedouble == static_cast<double>(expected));
        const cJSON* error = cJSON_GetObjectItemCaseSensitive(echoed.get(), "error");
        CHECK(cJSON_IsObject(error));
        CHECK(cJSON_GetObjectItemCaseSensitive(error, "code")->valueint == -32600);
    }
    CHECK(live_allocations == 0);
    CHECK(live_bytes == 0);
}

void test_rpc_request_envelope() {
    using tk::mcp_json::RpcIdKind;
    using tk::mcp_json::RpcIdStatus;
    using tk::mcp_json::RpcRequestStatus;

    reset_allocator(0);
    check_numeric_id_echo(
        "{\"jsonrpc\":\"2.0\",\"id\":9007199254740991,\"method\":\"ping\"}",
        tk::kJsonSafeIntegerMax,
        "{\"jsonrpc\":\"2.0\",\"id\":9007199254740991,\"error\":"
        "{\"code\":-32600,\"message\":\"fixture\"}}");
    check_numeric_id_echo(
        "{\"jsonrpc\":\"2.0\",\"id\":-9007199254740991,\"method\":\"ping\"}",
        -tk::kJsonSafeIntegerMax,
        "{\"jsonrpc\":\"2.0\",\"id\":-9007199254740991,\"error\":"
        "{\"code\":-32600,\"message\":\"fixture\"}}");
    check_numeric_id_echo(
        "{\"jsonrpc\":\"2.0\",\"\\u0069d\":17,\"method\":\"ping\"}", 17,
        "{\"jsonrpc\":\"2.0\",\"id\":17,\"error\":"
        "{\"code\":-32600,\"message\":\"fixture\"}}");
    check_numeric_id_echo(
        "{\"jsonrpc\":\"2.0\",\"i\\u0064\":18,\"method\":\"ping\"}", 18,
        "{\"jsonrpc\":\"2.0\",\"id\":18,\"error\":"
        "{\"code\":-32600,\"message\":\"fixture\"}}");

    for (const char* body : {
             "{\"jsonrpc\":\"2.0\",\"id\":9007199254740992,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":9007199254740993,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":-9007199254740992,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":1.5,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":1e-400,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":1e3,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":9007199254740990.5,"
             "\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":-0,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"\\u0069\\u0064\":1e3,"
             "\"method\":\"ping\"}"}) {
        const auto request = inspect_request(body);
        CHECK(request.status == RpcRequestStatus::InvalidId);
        CHECK(request.id_status == RpcIdStatus::Invalid);
        CHECK(request.id.kind == RpcIdKind::Missing);
    }

    for (double nonfinite : {std::nan(""), HUGE_VAL, -HUGE_VAL}) {
        {
            tk::JsonOwner value(cJSON_CreateNumber(nonfinite));
            CHECK(value != nullptr);
            tk::mcp_json::RpcId id;
            const tk::JsonRawNumberId invalid_raw{0, tk::JsonRawNumberStatus::InvalidNumber};
            CHECK(tk::mcp_json::capture_id(value.get(), invalid_raw, id) == RpcIdStatus::Invalid);
            CHECK(id.kind == RpcIdKind::Missing);
        }
        CHECK(live_allocations == 0);
    }

    for (const char* body : {
             "{\"id\":17,\"method\":\"ping\"}",
             "{\"jsonrpc\":\"1.0\",\"id\":17,\"method\":\"ping\"}",
             "{\"jsonrpc\":2.0,\"id\":17,\"method\":\"ping\"}"}) {
        const auto request = inspect_request(body);
        CHECK(request.status == RpcRequestStatus::InvalidVersion);
        CHECK(request.id_status == RpcIdStatus::Valid);
        CHECK(request.id.kind == RpcIdKind::Number && request.id.number == 17);
    }

    {
        const auto notification = inspect_request(
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
        CHECK(notification.status == RpcRequestStatus::Valid);
        CHECK(notification.id_status == RpcIdStatus::Missing);
        CHECK(notification.id.kind == RpcIdKind::Missing);
    }
    {
        const auto invalid_null = inspect_request(
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}");
        CHECK(invalid_null.status == RpcRequestStatus::InvalidId);
        CHECK(invalid_null.id_status == RpcIdStatus::Invalid);
        CHECK(invalid_null.id.kind == RpcIdKind::Missing);
    }

    for (const char* body : {
             "{\"jsonrpc\":\"2.0\",\"jsonrpc\":\"1.0\",\"id\":19,"
             "\"method\":\"ping\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"ping\","
             "\"method\":\"tools/call\"}",
             "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"set_charging_amps\",\"arguments\":"
             "{\"amps\":16,\"amps\":17}}}",
             "{\"jsonrpc\":\"2.0\",\"id\":22,\"\\u006dethod\":\"ping\","
             "\"method\":\"tools/list\"}"}) {
        const auto request = inspect_request(body);
        CHECK(request.status == RpcRequestStatus::DuplicateKey);
        CHECK(request.id_status == RpcIdStatus::Valid);
        CHECK(request.id.kind == RpcIdKind::Number);
    }
    {
        const auto duplicate_id = inspect_request(
            "{\"jsonrpc\":\"2.0\",\"id\":23,\"id\":24,\"method\":\"ping\"}");
        CHECK(duplicate_id.status == RpcRequestStatus::DuplicateKey);
        CHECK(duplicate_id.id_status == RpcIdStatus::Invalid);
        CHECK(duplicate_id.id.kind == RpcIdKind::Missing);
    }
    {
        const auto escaped_duplicate_id = inspect_request(
            "{\"jsonrpc\":\"2.0\",\"id\":25,\"\\u0069d\":26,"
            "\"method\":\"ping\"}");
        CHECK(escaped_duplicate_id.status == RpcRequestStatus::DuplicateKey);
        CHECK(escaped_duplicate_id.id_status == RpcIdStatus::Invalid);
        CHECK(escaped_duplicate_id.id.kind == RpcIdKind::Missing);
    }

    CHECK(live_allocations == 0);
    CHECK(live_bytes == 0);
}

std::string raw_string_document(const char* bytes, size_t length) {
    return std::string("{\"text\":\"") + std::string(bytes, length) + "\"}";
}

void test_raw_utf8_before_cjson() {
    static constexpr char valid_scalars[] =
        "\xc2\xa2" "\xe2\x82\xac" "\xf0\x90\x8d\x88";
    const std::string valid = raw_string_document(valid_scalars, sizeof(valid_scalars) - 1);
    reset_allocator(0);
    int parser_calls = 0;
    {
        const auto parsed = tk::json_materialize<cJSON>(
            valid.c_str(), valid.size(), [&](const char* text) {
                ++parser_calls;
                return cJSON_Parse(text);
            });
        CHECK(parsed.status == tk::JsonMaterializeStatus::Ok);
        tk::JsonOwner owner(parsed.root);
        CHECK(owner != nullptr);
    }
    CHECK(parser_calls == 1);
    CHECK(live_allocations == 0);

    struct InvalidUtf8 {
        const char* bytes;
        size_t length;
    };
    static constexpr char lone_continuation[] = "\x80";
    static constexpr char bad_lead[] = "\xc0\x80";
    static constexpr char truncated[] = "\xf0\x90\x80";
    static constexpr char overlong[] = "\xe0\x80\x80";
    static constexpr char surrogate[] = "\xed\xa0\x80";
    static constexpr char above_unicode[] = "\xf4\x90\x80\x80";
    const InvalidUtf8 invalid[] = {
        {lone_continuation, sizeof(lone_continuation) - 1},
        {bad_lead, sizeof(bad_lead) - 1},
        {truncated, sizeof(truncated) - 1},
        {overlong, sizeof(overlong) - 1},
        {surrogate, sizeof(surrogate) - 1},
        {above_unicode, sizeof(above_unicode) - 1},
    };
    for (const InvalidUtf8& raw : invalid) {
        const std::string body = raw_string_document(raw.bytes, raw.length);
        reset_allocator(0);
        parser_calls = 0;
        const auto parsed = tk::json_materialize<cJSON>(
            body.c_str(), body.size(), [&](const char* text) {
                ++parser_calls;
                return cJSON_Parse(text);
            });
        CHECK(parsed.status == tk::JsonMaterializeStatus::Malformed);
        CHECK(parsed.root == nullptr);
        CHECK(parser_calls == 0);
        CHECK(allocation_attempt == 0);
        CHECK(live_allocations == 0);
    }
}

void test_real_parser_oom() {
    static constexpr char body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"set_charging_amps\",\"arguments\":{\"charging_amps\":16}}}";

    reset_allocator(0);
    size_t allocation_count = 0;
    {
        const auto parsed = tk::json_materialize<cJSON>(
            body, std::strlen(body), [](const char* text) { return cJSON_Parse(text); });
        CHECK(parsed.status == tk::JsonMaterializeStatus::Ok);
        tk::JsonOwner owner(parsed.root);
        allocation_count = allocation_attempt;
    }
    CHECK(live_allocations == 0);

    for (size_t nth = 1; nth <= allocation_count; ++nth) {
        reset_allocator(nth);
        const auto parsed = tk::json_materialize<cJSON>(
            body, std::strlen(body), [](const char* text) { return cJSON_Parse(text); });
        CHECK(parsed.status == tk::JsonMaterializeStatus::NoMemory);
        CHECK(parsed.root == nullptr);
        CHECK(live_allocations == 0);
    }
    std::cout << "  real cJSON_Parse: " << allocation_count << " allocation stages\n";
}

void test_embedded_nul_rejected_before_cjson() {
    static constexpr char body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"a\\u0000b\",\"method\":\"tools/list\"}";
    reset_allocator(0);
    int parser_calls = 0;
    const auto parsed = tk::json_materialize<cJSON>(
        body, std::strlen(body), [&](const char* text) {
            ++parser_calls;
            return cJSON_Parse(text);
        });
    CHECK(parsed.status == tk::JsonMaterializeStatus::UnsupportedNul);
    CHECK(parsed.root == nullptr);
    CHECK(parser_calls == 0);
    CHECK(allocation_attempt == 0);
    CHECK(live_allocations == 0);
}

void test_ota_manifest_inspector() {
    using Status = tk::OtaManifestInspectStatus;
    const auto inspect_status = [](const char* body) {
        tk::JsonOwner root(cJSON_Parse(body));
        CHECK(root != nullptr);
        return tk::inspect_ota_manifest(root.get()).status;
    };

    reset_allocator(0);
    {
        tk::JsonOwner root(cJSON_Parse("{\"version\":\"1.2.3\",\"ignored\":true}"));
        CHECK(root != nullptr);
        const auto manifest = tk::inspect_ota_manifest(root.get());
        CHECK(manifest.status == Status::Valid);
        CHECK(manifest.value != nullptr && std::strcmp(manifest.value, "1.2.3") == 0);
    }
    CHECK(inspect_status("[]") == Status::ObjectRequired);
    CHECK(inspect_status("{}") == Status::VersionRequired);
    CHECK(inspect_status("{\"version\":1}") == Status::VersionRequired);
    CHECK(inspect_status("{\"version\":\"1.2.3\",\"version\":\"2.0.0\"}") ==
          Status::DuplicateKey);
    CHECK(inspect_status("{\"version\":\"1.2.3\",\"ignored\":1,\"ignored\":2}") ==
          Status::DuplicateKey);
    CHECK(inspect_status("{\"version\":\"1.2.3\",\"ver\\u0073ion\":\"2.0.0\"}") ==
          Status::DuplicateKey);
    CHECK(live_allocations == 0);
}

void test_throw_ownership() {
    static constexpr char body[] =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{}}";
    const tk::JsonRawNumberId raw_numeric_id =
        tk::json_top_level_numeric_id(body, std::strlen(body));
    struct FixtureThrow : std::runtime_error {
        FixtureThrow() : std::runtime_error("fixture") {}
    };

    for (int stage = 0; stage < 3; ++stage) {
        reset_allocator(0);
        try {
            tk::JsonOwner request(cJSON_Parse(body));
            CHECK(request != nullptr);
            if (stage == 0) throw FixtureThrow();
            tk::mcp_json::RpcId id;
            CHECK(tk::mcp_json::capture_id(
                      cJSON_GetObjectItemCaseSensitive(request.get(), "id"), raw_numeric_id, id)
                  == tk::mcp_json::RpcIdStatus::Valid);
            if (stage == 1) throw FixtureThrow();
            request.reset();  // mirrors tools/call after fixed argument extraction
            if (stage == 2) throw FixtureThrow();
        } catch (const FixtureThrow&) {
        }
        CHECK(live_allocations == 0);
    }
}

}  // namespace

int main() {
    cJSON_Hooks hooks{tracked_malloc, tracked_free};
    cJSON_InitHooks(&hooks);

    static constexpr char rest_oom[] =
        "{\"result\":false,\"reason\":\"out of memory\"}";
    static constexpr char mcp_oom[] =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":"
        "{\"code\":-32603,\"message\":\"out of memory\"}}";
    exhaust_build_and_print("full /status production emitter", build_status_fixture, rest_oom);
    exhaust_build_and_print("REST production reply", build_rest_fixture, rest_oom);
    exhaust_build_and_print("MCP result production reply", build_mcp_result_fixture, mcp_oom);
    exhaust_build_and_print(
        "MCP max-safe-id exact reply", build_mcp_max_safe_id_fixture, mcp_oom);
    exhaust_build_and_print("MCP error production reply", build_mcp_error_fixture, mcp_oom);
    exhaust_build_and_print("MCP tools/list production reply", build_tools_list_fixture, mcp_oom);
    exhaust_build_and_print(
        "MCP vehicle-state double-print production reply", build_vehicle_state_fixture, mcp_oom);
    test_status_emitter_structure_guards();
    test_rpc_request_envelope();
    test_raw_utf8_before_cjson();
    test_real_parser_oom();
    test_embedded_nul_rejected_before_cjson();
    test_ota_manifest_inspector();
    test_throw_ownership();
    test_max_tools_list_input_lifetime();

    reset_allocator(0);
    cJSON_InitHooks(nullptr);
    std::cout << "OK " << checks << " cJSON OOM/ownership checks passed\n";
    return 0;
}
