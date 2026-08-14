#pragma once

#include <cstddef>
#include <string_view>

namespace tk {

enum class WifiCredentialError {
    None,
    SsidLength,
    EmbeddedNul,
    PasswordLength,
    RawPskNotHex,
};

inline bool wifi_is_hex_psk(std::string_view password) {
    if (password.size() != 64) return false;
    for (unsigned char c : password) {
        const bool hex = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// One validation contract for the setup AP and the authenticated/LAN configuration endpoint.
// Password forms accepted by ESP-IDF are: empty for an open AP, an 8..63-byte passphrase, or a
// 64-hex-digit raw WPA PSK. Embedded NUL is rejected because every downstream ESP/NVS API is
// C-string based and would otherwise persist/connect with a silently truncated credential.
inline WifiCredentialError wifi_credentials_error(std::string_view ssid,
                                                   std::string_view password) {
    if (ssid.empty() || ssid.size() > 32) return WifiCredentialError::SsidLength;
    if (ssid.find('\0') != std::string_view::npos ||
        password.find('\0') != std::string_view::npos) return WifiCredentialError::EmbeddedNul;
    if (password.empty()) return WifiCredentialError::None;
    if (password.size() == 64)
        return wifi_is_hex_psk(password) ? WifiCredentialError::None
                                         : WifiCredentialError::RawPskNotHex;
    if (password.size() < 8 || password.size() > 63)
        return WifiCredentialError::PasswordLength;
    return WifiCredentialError::None;
}

inline const char* wifi_credentials_reason(WifiCredentialError error) {
    switch (error) {
        case WifiCredentialError::None:          return "";
        case WifiCredentialError::SsidLength:    return "SSID must be 1-32 bytes";
        case WifiCredentialError::EmbeddedNul:   return "SSID/password contains an invalid NUL byte";
        case WifiCredentialError::PasswordLength:return "password must be empty, 8-63 bytes, or a 64-hex PSK";
        case WifiCredentialError::RawPskNotHex:  return "a 64-byte WiFi PSK must contain hexadecimal digits only";
    }
    return "invalid WiFi credentials";
}

}  // namespace tk
