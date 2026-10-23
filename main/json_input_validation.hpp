#pragma once

#include "cJSON.h"
#include "logic/json_syntax.hpp"
#include <cstring>

// Shared cJSON input checks. The request syntax gate bounds depth and values before this walk;
// object names are compared after cJSON decoding, with their original case sensitivity.
namespace tk {

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

// JSON numbers retain their existing finite/integral/range checks in each command adapter.
// Numeric strings use only the canonical decimal subset; never strtod's whitespace/hex/exponent
// extensions. The safe-integer range covers every command-registry int range exactly.
inline bool json_command_number(const cJSON* value, double& out) noexcept {
    if (cJSON_IsNumber(value)) {
        out = value->valuedouble;
        return true;
    }
    if (!cJSON_IsString(value) || !value->valuestring) return false;
    const char* first = value->valuestring;
    int64_t integer{};
    if (!json_canonical_safe_integer(first, first + std::strlen(first), integer)) return false;
    out = static_cast<double>(integer);
    return true;
}

}  // namespace tk
