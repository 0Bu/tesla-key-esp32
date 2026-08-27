#pragma once

#include <array>
#include <string_view>

namespace tk {

enum class HttpVerb { Get, Post, Other };

enum class HttpRoute {
    NotFound,
    Command,
    VehicleData,
    BodyController,
    OtaCheck,
    OtaUpdate,
    OtaStatus,
    GenKeys,
    SendKey,
    SetTime,
    SetVin,
    SetMqtt,
    SetSyslog,
    SetWifi,
    Scan,
    Coredump,
    CrashDismiss,
    Heap,
    McpPost,
    McpGet,
    Version,
    Status,
    Diag,
    Index,
};

struct FixedHttpRoute {
    HttpVerb verb;
    std::string_view path;
    HttpRoute route;
};

inline constexpr std::array<FixedHttpRoute, 21> kFixedHttpRoutes{{
    {HttpVerb::Get,  "/ota/check",           HttpRoute::OtaCheck},
    {HttpVerb::Post, "/ota/update",          HttpRoute::OtaUpdate},
    {HttpVerb::Get,  "/ota/status",          HttpRoute::OtaStatus},
    {HttpVerb::Post, "/gen_keys",            HttpRoute::GenKeys},
    {HttpVerb::Post, "/send_key",            HttpRoute::SendKey},
    {HttpVerb::Post, "/set_time",            HttpRoute::SetTime},
    {HttpVerb::Post, "/set_vin",             HttpRoute::SetVin},
    {HttpVerb::Post, "/set_mqtt",            HttpRoute::SetMqtt},
    {HttpVerb::Post, "/set_syslog",          HttpRoute::SetSyslog},
    {HttpVerb::Post, "/set_wifi",            HttpRoute::SetWifi},
    {HttpVerb::Post, "/scan",                HttpRoute::Scan},
    {HttpVerb::Get,  "/coredump",            HttpRoute::Coredump},
    {HttpVerb::Post, "/crash/dismiss",       HttpRoute::CrashDismiss},
    {HttpVerb::Get,  "/heap",                HttpRoute::Heap},
    {HttpVerb::Post, "/mcp",                 HttpRoute::McpPost},
    {HttpVerb::Get,  "/mcp",                 HttpRoute::McpGet},
    {HttpVerb::Get,  "/api/proxy/1/version", HttpRoute::Version},
    {HttpVerb::Get,  "/status",              HttpRoute::Status},
    {HttpVerb::Get,  "/diag",                HttpRoute::Diag},
    {HttpVerb::Get,  "/",                    HttpRoute::Index},
    {HttpVerb::Get,  "/index.html",           HttpRoute::Index},
}};

inline constexpr std::string_view kVehicleRoutePrefix = "/api/1/vehicles/";

inline constexpr std::string_view http_path_only(std::string_view uri) noexcept {
    const size_t query = uri.find('?');
    return query == std::string_view::npos ? uri : uri.substr(0, query);
}

inline constexpr bool http_single_segment(std::string_view value) noexcept {
    return !value.empty() && value.find('/') == std::string_view::npos;
}

inline constexpr HttpRoute classify_vehicle_route(HttpVerb verb,
                                                   std::string_view path) noexcept {
    if (path.size() <= kVehicleRoutePrefix.size() ||
        path.substr(0, kVehicleRoutePrefix.size()) != kVehicleRoutePrefix) {
        return HttpRoute::NotFound;
    }
    const std::string_view rest = path.substr(kVehicleRoutePrefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos || !http_single_segment(rest.substr(0, slash))) {
        return HttpRoute::NotFound;
    }
    const std::string_view suffix = rest.substr(slash);
    if (verb == HttpVerb::Get && suffix == "/vehicle_data") {
        return HttpRoute::VehicleData;
    }
    if (verb == HttpVerb::Get && suffix == "/body_controller_state") {
        return HttpRoute::BodyController;
    }
    constexpr std::string_view command = "/command/";
    if (verb == HttpVerb::Post && suffix.size() > command.size() &&
        suffix.substr(0, command.size()) == command &&
        http_single_segment(suffix.substr(command.size()))) {
        return HttpRoute::Command;
    }
    return HttpRoute::NotFound;
}

inline constexpr HttpRoute classify_http_route(HttpVerb verb,
                                                std::string_view path) noexcept {
    const HttpRoute vehicle = classify_vehicle_route(verb, path);
    if (vehicle != HttpRoute::NotFound) return vehicle;
    for (const FixedHttpRoute& route : kFixedHttpRoutes) {
        if (route.verb == verb && route.path == path) return route.route;
    }
    return HttpRoute::NotFound;
}

// Routes that can scan, connect, pair, mutate vehicle identity or issue a signed command are
// unavailable until the whole essential runtime is Ready.  MCP POST is intentionally classified
// as one unit: method parsing inside a wildcard endpoint must never become an admission bypass.
inline constexpr bool http_route_requires_vehicle_runtime(HttpRoute route) noexcept {
    switch (route) {
        case HttpRoute::Command:
        case HttpRoute::VehicleData:
        case HttpRoute::BodyController:
        case HttpRoute::GenKeys:
        case HttpRoute::SendKey:
        case HttpRoute::SetVin:
        case HttpRoute::Scan:
        case HttpRoute::McpPost:
            return true;
        case HttpRoute::NotFound:
        case HttpRoute::OtaCheck:
        case HttpRoute::OtaUpdate:
        case HttpRoute::OtaStatus:
        case HttpRoute::SetTime:
        case HttpRoute::SetMqtt:
        case HttpRoute::SetSyslog:
        case HttpRoute::SetWifi:
        case HttpRoute::Coredump:
        case HttpRoute::CrashDismiss:
        case HttpRoute::Heap:
        case HttpRoute::McpGet:
        case HttpRoute::Version:
        case HttpRoute::Status:
        case HttpRoute::Diag:
        case HttpRoute::Index:
            return false;
    }
    return true;
}

}  // namespace tk
