#pragma once

// Shared DHCP/mDNS boot name. HTTP ownership checks also accept the live mDNS name after a
// conflict rename. Pure C++17; tested through http_origin.hpp in test/test_logic.cpp.
namespace tk {
inline constexpr char kDeviceHostname[] = "tesla-key-esp32";
}  // namespace tk
