#pragma once

#include <cstdint>
#include "config_commit.hpp"

// Pure, hardware-free logic shared by the firmware and the host-side mock build.
// Keep free of IDF/FreeRTOS/NimBLE/NVS/cJSON/esp_http_server includes.
// http_effect_reply.hpp and the mutation handlers delegate here. See test/README.md.
namespace tk {

enum class RequestOutcome : uint8_t { NotAttempted, Uncertain, Succeeded, IdentityRecovery };
enum class RequestEffect : uint8_t {
    Command = 1, Configuration = 2, Clock = 4, Identity = 8, OtaStart = 16, CoredumpErase = 32
};
enum class ReplyShape : uint8_t { WrappedRest, FlatResult, OtaCheck, Rpc };
enum class ReplyContinuation : uint8_t { None, IdentityRestart, SuccessfulConfigRestart };

struct EffectReplyState {
    RequestOutcome outcome{RequestOutcome::NotAttempted};
    ReplyShape shape{ReplyShape::WrappedRest};
    ReplyContinuation continuation{ReplyContinuation::None};
    uint8_t possible_effects{};
    bool recovery_pending{};
    bool terminal{};
    bool continuation_consumed{};
    ConfigImageResult image{ConfigImageResult::None};

    void attempting(RequestEffect effect) noexcept {
        possible_effects |= static_cast<uint8_t>(effect);
        outcome = RequestOutcome::Uncertain;
    }
    // Only a typed refusal proving this particular effect was never attempted may clear it.
    // Preserve uncertainty from any other effect in the same request.
    void refused(RequestEffect effect) noexcept {
        possible_effects &= ~static_cast<uint8_t>(effect);
        if (!possible_effects && outcome == RequestOutcome::Uncertain)
            outcome = RequestOutcome::NotAttempted;
    }
    void succeeded() noexcept { outcome = RequestOutcome::Succeeded; }
    void identity_recovery() noexcept {
        outcome = RequestOutcome::IdentityRecovery;
        recovery_pending = true;
    }
    bool needs_effect_reply() const noexcept {
        return possible_effects != 0 || outcome != RequestOutcome::NotAttempted;
    }
    bool success() const noexcept { return outcome == RequestOutcome::Succeeded; }
    const char* outcome_text() const noexcept {
        return success() ? "succeeded"
             : outcome == RequestOutcome::IdentityRecovery ? "applied_recovery_pending"
                                                          : "unknown";
    }
    const char* reason() const noexcept {
        if (success() && config_image_warning(image)) return config_commit_reason(image);
        return success() ? "request succeeded; reply degraded"
             : outcome == RequestOutcome::IdentityRecovery
                 ? "identity saved; recovery pending; reply degraded"
                 : "operation outcome unknown; reply degraded";
    }
    // Consume BEFORE calling out. A transport failure/exception cannot re-run a continuation.
    ReplyContinuation take_continuation() noexcept {
        if (continuation_consumed) return ReplyContinuation::None;
        continuation_consumed = true;
        return continuation;
    }
};

static_assert(sizeof(EffectReplyState) <= 8, "request outcome must remain small");

}  // namespace tk
