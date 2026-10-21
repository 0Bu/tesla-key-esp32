#pragma once
#include <cstddef>

// New HTTP/UI submissions share the old config writer's limit. Existing supported legacy
// targets may be preserved through unrelated saves; that does not make older writers compatible.
namespace tk {
inline constexpr size_t kServiceTargetInputMaxBytes = 64;
inline constexpr size_t kServiceTargetPreserveMaxBytes = 120;
static_assert(kServiceTargetInputMaxBytes <= kServiceTargetPreserveMaxBytes);
} // namespace tk
