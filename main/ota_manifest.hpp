#pragma once

#include <cJSON.h>
#include "logic/ota_contract.hpp"

#include <cstddef>
#include <cstring>

namespace tk {

enum class OtaManifestInspectStatus {
    Valid,
    ObjectRequired,
    DuplicateKey,
    VersionRequired,
};

struct OtaManifestVersion {
    const char* value{};
    OtaManifestInspectStatus status{OtaManifestInspectStatus::ObjectRequired};
};

inline bool ota_manifest_unique_object(const cJSON* root) noexcept {
    if (!cJSON_IsObject(root)) return false;
    for (const cJSON* item = root->child; item; item = item->next) {
        if (!item->string) return false;
        for (const cJSON* later = item->next; later; later = later->next)
            if (!later->string || std::strcmp(item->string, later->string) == 0) return false;
    }
    return true;
}

// cJSON preserves duplicate members and its first-match lookup would make a manifest such as
// {"version":"2.0.0","version":"1.0.0"} ambiguous. The allocation-free syntax gate has
// already rejected excessive nesting and U+0000 before this inspector runs. Require a root object,
// reject every duplicate root key (including escape-equivalent spellings after cJSON decoding),
// and expose exactly one string-valued version.
inline OtaManifestVersion inspect_ota_manifest(const cJSON* root) noexcept {
    OtaManifestVersion out;
    if (!cJSON_IsObject(root)) return out;

    if (!ota_manifest_unique_object(root)) {
        out.status = OtaManifestInspectStatus::DuplicateKey;
        return out;
    }
    const cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsString(version) || !version->valuestring) {
        out.status = OtaManifestInspectStatus::VersionRequired;
        return out;
    }
    out.value = version->valuestring;
    out.status = OtaManifestInspectStatus::Valid;
    return out;
}

struct OtaManifestImage {
    std::array<uint8_t, 32> sha256{};
    uint32_t bytes{0};
};

// The URL remains firmware-owned. Select one unambiguous app part from the already bounded
// parsed manifest; never turn a supplied path into an arbitrary download or partition target.
inline bool inspect_ota_image(const cJSON* root, const char* app_path,
                              OtaManifestImage& out) noexcept {
    out = {};
    const auto* builds = cJSON_GetObjectItemCaseSensitive(root, "builds");
    if (!app_path || !cJSON_IsArray(builds)) return false;
    bool found = false;
    for (const auto* build = builds->child; build; build = build->next) {
        if (!ota_manifest_unique_object(build)) return false;
        const auto* parts = cJSON_GetObjectItemCaseSensitive(build, "parts");
        if (!cJSON_IsArray(parts)) return false;
        for (const auto* part = parts->child; part; part = part->next) {
            if (!ota_manifest_unique_object(part)) return false;
            const auto* path = cJSON_GetObjectItemCaseSensitive(part, "path");
            if (!cJSON_IsString(path) || !path->valuestring) return false;
            if (std::strcmp(path->valuestring, app_path) != 0) continue;
            if (found) return false;
            const auto* size = cJSON_GetObjectItemCaseSensitive(part, "size");
            const auto* offset = cJSON_GetObjectItemCaseSensitive(part, "offset");
            const auto* digest = cJSON_GetObjectItemCaseSensitive(part, "sha256");
            if (!cJSON_IsNumber(size) || !(size->valuedouble >= 512 &&
                size->valuedouble <= kOtaUploadMaxBytes) ||
                static_cast<uint32_t>(size->valuedouble) != size->valuedouble ||
                !cJSON_IsNumber(offset) || offset->valuedouble != 0x20000 ||
                !cJSON_IsString(digest) || !digest->valuestring ||
                !parse_ota_sha256(digest->valuestring, out.sha256)) return false;
            out.bytes = static_cast<uint32_t>(size->valuedouble);
            found = true;
        }
    }
    return found;
}

}  // namespace tk
