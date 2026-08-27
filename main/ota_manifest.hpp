#pragma once

#include <cJSON.h>

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

// cJSON preserves duplicate members and its first-match lookup would make a manifest such as
// {"version":"2.0.0","version":"1.0.0"} ambiguous. The allocation-free syntax gate has
// already rejected excessive nesting and U+0000 before this inspector runs. Require a root object,
// reject every duplicate root key (including escape-equivalent spellings after cJSON decoding),
// and expose exactly one string-valued version.
inline OtaManifestVersion inspect_ota_manifest(const cJSON* root) noexcept {
    OtaManifestVersion out;
    if (!cJSON_IsObject(root)) return out;

    const cJSON* version = nullptr;
    for (const cJSON* item = root->child; item; item = item->next) {
        if (!item->string) {
            out.status = OtaManifestInspectStatus::DuplicateKey;
            return out;
        }
        for (const cJSON* later = item->next; later; later = later->next) {
            if (!later->string || std::strcmp(item->string, later->string) == 0) {
                out.status = OtaManifestInspectStatus::DuplicateKey;
                return out;
            }
        }
        if (std::strcmp(item->string, "version") == 0) version = item;
    }
    if (!cJSON_IsString(version) || !version->valuestring) {
        out.status = OtaManifestInspectStatus::VersionRequired;
        return out;
    }
    out.value = version->valuestring;
    out.status = OtaManifestInspectStatus::Valid;
    return out;
}

}  // namespace tk
