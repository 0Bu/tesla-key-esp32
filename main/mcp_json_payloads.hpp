#pragma once

#include "json_builder.hpp"
#include "json_http_reply.hpp"
#include "logic/command_registry.hpp"
#include "logic/json_syntax.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>

namespace tk::mcp_json {

// JSON-RPC ids are echoed, but an attacker-controlled 2-KiB id must not pin an input-sized cJSON
// subtree while tools/list builds its own ~1.5-KiB contiguous print buffer. Accept the protocol's
// string/number forms, cap strings explicitly, and reduce them to this fixed record before the
// request tree is released. Invalid/oversized ids receive Invalid Request with a null id.
inline constexpr size_t kMaxStringIdBytes = 64;

enum class RpcIdKind { Missing, Number, String };
enum class RpcIdStatus { Missing, Valid, Invalid };

struct RpcId {
    int64_t number{};
    char string[kMaxStringIdBytes + 1]{};
    RpcIdKind kind{RpcIdKind::Missing};
};

static_assert(sizeof(RpcId) <= 80, "RPC id snapshot must remain fixed and small");

inline RpcIdStatus capture_id(const cJSON* value, const JsonRawNumberId& raw_number,
                              RpcId& out) noexcept {
    out = {};
    if (!value) return RpcIdStatus::Missing;
    // JSON-RPC discourages null as a request id because it is indistinguishable from the id used
    // when an error cannot be correlated. Treat explicit null as invalid; only true absence is a
    // notification/client-response candidate.
    if (cJSON_IsNull(value)) return RpcIdStatus::Invalid;
    if (cJSON_IsNumber(value)) {
        int64_t materialized = 0;
        if (raw_number.status != JsonRawNumberStatus::ValidInteger ||
            !json_safe_integer(value->valuedouble, materialized) ||
            materialized != raw_number.value) {
            return RpcIdStatus::Invalid;
        }
        out.number = raw_number.value;
        out.kind = RpcIdKind::Number;
        return RpcIdStatus::Valid;
    }
    if (!cJSON_IsString(value) || !value->valuestring) return RpcIdStatus::Invalid;

    size_t length = 0;
    while (length <= kMaxStringIdBytes && value->valuestring[length] != '\0') ++length;
    if (length > kMaxStringIdBytes) return RpcIdStatus::Invalid;
    std::memcpy(out.string, value->valuestring, length + 1);
    out.kind = RpcIdKind::String;
    return RpcIdStatus::Valid;
}

inline void emit_id(JsonBuilder& json, const RpcId& id) noexcept {
    if (id.kind == RpcIdKind::Number)
        json.integer(json.root(), "id", id.number);
    else if (id.kind == RpcIdKind::String) json.string(json.root(), "id", id.string);
    else json.null(json.root(), "id");
}

inline size_t object_key_count(const cJSON* object, const char* key) noexcept {
    if (!cJSON_IsObject(object) || !key) return 0;
    size_t count = 0;
    for (const cJSON* item = object->child; item; item = item->next) {
        if (item->string && std::strcmp(item->string, key) == 0) ++count;
    }
    return count;
}

// cJSON preserves duplicate object members and its lookup API returns only one of them. Reject
// duplicates recursively before routing so a second jsonrpc/id/method (including an escaped-key
// spelling decoded to the same string) cannot select a different interpretation. The walk is
// allocation-free and the syntax gate already caps nesting at 16 containers.
inline bool has_duplicate_json_keys(const cJSON* value) noexcept {
    if (!value) return false;
    if (cJSON_IsObject(value)) {
        for (const cJSON* item = value->child; item; item = item->next) {
            if (!item->string) return true;
            for (const cJSON* later = item->next; later; later = later->next) {
                if (!later->string || std::strcmp(item->string, later->string) == 0) return true;
            }
        }
    }
    if (cJSON_IsObject(value) || cJSON_IsArray(value)) {
        for (const cJSON* item = value->child; item; item = item->next) {
            if (has_duplicate_json_keys(item)) return true;
        }
    }
    return false;
}

enum class RpcRequestStatus { Valid, InvalidObject, DuplicateKey, InvalidId, InvalidVersion };

struct RpcRequestEnvelope {
    RpcId id{};
    RpcIdStatus id_status{RpcIdStatus::Missing};
    RpcRequestStatus status{RpcRequestStatus::InvalidObject};
};

// Inspect the common JSON-RPC envelope before notification detection or method dispatch. A unique,
// valid id is retained for an Invalid Request response even when jsonrpc or another key is wrong;
// duplicate ids are inherently ambiguous and therefore correlate as null.
inline RpcRequestEnvelope inspect_request_envelope(
    const cJSON* object, const JsonRawNumberId& raw_number) noexcept {
    RpcRequestEnvelope out;
    if (!cJSON_IsObject(object)) return out;

    const size_t id_count = object_key_count(object, "id");
    if (id_count <= 1) {
        out.id_status = capture_id(
            id_count == 1 ? cJSON_GetObjectItemCaseSensitive(object, "id") : nullptr,
            raw_number, out.id);
    }

    if (has_duplicate_json_keys(object)) {
        if (id_count != 1 || out.id_status != RpcIdStatus::Valid) {
            out.id = {};
            out.id_status = id_count == 0 ? RpcIdStatus::Missing : RpcIdStatus::Invalid;
        }
        out.status = RpcRequestStatus::DuplicateKey;
        return out;
    }
    if (out.id_status == RpcIdStatus::Invalid) {
        out.id = {};
        out.status = RpcRequestStatus::InvalidId;
        return out;
    }

    const cJSON* version = cJSON_GetObjectItemCaseSensitive(object, "jsonrpc");
    if (!cJSON_IsString(version) || !version->valuestring ||
        std::strcmp(version->valuestring, "2.0") != 0) {
        out.status = RpcRequestStatus::InvalidVersion;
        return out;
    }
    out.status = RpcRequestStatus::Valid;
    return out;
}

inline JsonOwner build_result_envelope(const RpcId& id, JsonOwner result) noexcept {
    if (!result) return {};
    JsonBuilder json;
    json.string(json.root(), "jsonrpc", "2.0");
    emit_id(json, id);
    json.adopt_object(json.root(), "result", std::move(result));
    return json.finish();
}

inline JsonOwner build_error_envelope(const RpcId& id, int code, const char* message) noexcept {
    if (!message) return {};
    JsonBuilder json;
    json.string(json.root(), "jsonrpc", "2.0");
    emit_id(json, id);
    cJSON* error = json.object(json.root(), "error");
    json.number(error, "code", code);
    json.string(error, "message", message);
    return json.finish();
}

inline JsonOwner build_tool_result(const char* text, bool is_error) noexcept {
    if (!text) return {};
    JsonBuilder json;
    cJSON* content = json.array(json.root(), "content");
    cJSON* block = json.object_element(content);
    json.string(block, "type", "text");
    json.string(block, "text", text);
    json.boolean(json.root(), "isError", is_error);
    return json.finish();
}

struct VehicleStatePayload {
    const char* vin{};
    bool paired{};
    const char* link{};
    bool has_last_seen{};
    double last_seen_s{};
    bool has_soc{};
    double soc{};
    const char* charging_state{};
    bool has_charge_limit{};
    double charge_limit{};
    bool has_charge_amps{};
    double charge_amps{};
    bool has_charger_power{};
    double charger_power_kw{};
};

inline JsonOwner build_vehicle_state_object(const VehicleStatePayload& in) noexcept {
    if (!in.vin || !in.link) return {};
    JsonBuilder json;
    json.string(json.root(), "vin", in.vin);
    json.boolean(json.root(), "paired", in.paired);
    json.string(json.root(), "link", in.link);
    if (in.has_last_seen) json.number(json.root(), "last_seen_s", in.last_seen_s);
    if (in.has_soc) json.number(json.root(), "soc", in.soc);
    if (in.charging_state && in.charging_state[0] != '\0')
        json.string(json.root(), "charging_state", in.charging_state);
    if (in.has_charge_limit) json.number(json.root(), "charge_limit", in.charge_limit);
    if (in.has_charge_amps) json.number(json.root(), "charge_amps", in.charge_amps);
    if (in.has_charger_power)
        json.number(json.root(), "charger_power_kw", in.charger_power_kw);
    return json.finish();
}

// The MCP content block is text, so this path has two contiguous print stages: the cached state
// object is printed into the text field, then the complete JSON-RPC envelope is printed by the HTTP
// reply seam. Keeping both stages here lets the pinned-cJSON n-th gate execute the exact producer.
inline JsonOwner build_vehicle_state_result(const VehicleStatePayload& in) noexcept {
    JsonOwner state = build_vehicle_state_object(in);
    if (!state) return {};
    JsonPrintOwner text(cJSON_PrintUnformatted(state.get()));
    if (!text) return {};
    return build_tool_result(text.get(), false);
}

inline JsonOwner build_tool_schema(const CmdInfo& info) noexcept {
    JsonBuilder json;
    json.string(json.root(), "type", "object");
    cJSON* props = json.object(json.root(), "properties");
    cJSON* required = nullptr;
    for (const auto& arg : info.args) {
        if (arg.type == CmdArgType::None || !arg.mcp_key) continue;
        cJSON* property = json.object(props, arg.mcp_key);
        if (arg.type == CmdArgType::Int) {
            json.string(property, "type", "integer");
            json.number(property, "minimum", arg.lo);
            json.number(property, "maximum", arg.hi);
        } else {
            json.string(property, "type", "boolean");
        }
        if (arg.mcp_required) {
            if (!required) required = json.array(json.root(), "required");
            json.string_reference_element(required, arg.mcp_key);
        }
    }
    return json.finish();
}

inline JsonOwner build_tools_list_result() noexcept {
    JsonBuilder json;
    cJSON* tools = json.array(json.root(), "tools");
    for (const auto& command : kCommands) {
        if (!command.mcp_name) continue;
        cJSON* tool = json.object_element(tools);
        json.string_reference(tool, "name", command.mcp_name);
        json.string_reference(tool, "description", command.mcp_desc);
        json.adopt_object(tool, "inputSchema", build_tool_schema(command));
    }
    return json.finish();
}

}  // namespace tk::mcp_json
