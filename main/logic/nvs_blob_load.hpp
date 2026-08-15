#pragma once

#include <cstddef>
#include <cstdint>

namespace tk {

enum class NvsBlobProbe : uint8_t {
    Ok,
    NotFound,
    Error,
};

enum class NvsBlobLoadState : uint8_t {
    Error,
    Missing,
    Present,
};

// Pure fault-injection contract for a two-call nvs_get_blob read. Only an exact NOT_FOUND probe is
// absence. ESP_OK with zero bytes is a present-but-invalid record, and a second read whose length
// changes cannot be treated as the snapshot that was probed first.
constexpr NvsBlobLoadState classify_nvs_blob_load(NvsBlobProbe probe,
                                                   size_t probed_len,
                                                   bool read_ok,
                                                   size_t read_len) {
    if (probe == NvsBlobProbe::NotFound) return NvsBlobLoadState::Missing;
    if (probe != NvsBlobProbe::Ok || probed_len == 0 || !read_ok || read_len != probed_len)
        return NvsBlobLoadState::Error;
    return NvsBlobLoadState::Present;
}

}  // namespace tk
