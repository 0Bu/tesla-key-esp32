#pragma once

#include <cstddef>
#include <cstdint>

namespace tk {

enum class NvsStringProbe : uint8_t {
    Ok,
    NotFound,
    Error,
};

enum class NvsStringLoadState : uint8_t {
    Error,
    Missing,
    Present,
};

// Pure fault-injection contract for NvsStorageAdapter::load_str_state(). NVS reports string
// lengths including the trailing NUL, hence a valid nonempty value needs at least two bytes.
// Legacy configuration alone opts into an exact one-byte, explicitly empty value. The
// second read must report exactly the probed length: accepting a shorter but still nonempty value
// could turn a different/torn journal into a syntactically valid recovery authority.
// Only NOT_FOUND is absence; every malformed or incomplete read remains a safety error.
constexpr NvsStringLoadState classify_nvs_string_load(NvsStringProbe probe,
                                                       size_t probed_len,
                                                       bool read_ok,
                                                       size_t read_len,
                                                       bool value_well_formed,
                                                       bool allow_empty = false) {
    if (probe == NvsStringProbe::NotFound) return NvsStringLoadState::Missing;
    if (probe != NvsStringProbe::Ok || probed_len == 0 ||
        (!allow_empty && probed_len == 1) || !read_ok ||
        read_len != probed_len || !value_well_formed)
        return NvsStringLoadState::Error;
    return NvsStringLoadState::Present;
}

}  // namespace tk
