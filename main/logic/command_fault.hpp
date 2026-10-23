#pragma once

#include "command_result.hpp"
#include <cstdint>

// Pure, bounded decisions shared by CommandRunner and its native fault adapter. No IDF,
// transport, protobuf or storage dependencies. The real-library harness pins enum numbers.
namespace tk {

enum class CommandEffect : uint8_t { Mutating, ReadOnly };
struct CommandFailureInfo {
    CommandError::Severity severity{CommandError::Severity::Unknown};
    CommandError::Outcome outcome{CommandError::Outcome::DefinitelyFailed};
};
inline constexpr bool execution_uncertain(CommandFailureInfo info) noexcept {
    return info.outcome != CommandError::Outcome::DefinitelyFailed;
}
inline constexpr CommandFailureInfo uncertain_command_failure() noexcept {
    return {CommandError::Severity::Unknown, CommandError::Outcome::MayHaveSucceeded};
}
inline constexpr bool wake_confirmation_required(bool send_success, CommandFailureInfo failure) noexcept {
    return send_success || execution_uncertain(failure);
}
struct MessageFaultDecision {
    bool known{true};
    bool temporary{false};
    bool possible_success{false};
    bool repair_session{false};
    const char* reason{"vehicle protocol fault"};
};
// vehicle-command 83baa078 pkg/protocol/error.go. NONE is not by itself an error;
// callers separately evaluate the outer operation status (which is not GCM-AAD-bound).
inline constexpr MessageFaultDecision classify_message_fault(uint32_t code) noexcept {
    switch (code) {
        case 0: return {false, false, true, false, "vehicle fault: unconfirmed vehicle operation status"};
        case 1: return {true, true, false, false, "vehicle fault: vehicle busy"};
        case 2: return {true, true, false, false, "vehicle fault: vehicle subsystem timeout"};
        case 3: return {true, false, false, false, "vehicle fault: key not on whitelist"};
        case 4: return {true, false, false, false, "vehicle fault: vehicle key inactive"};
        case 5: return {true, true, false, true, "vehicle fault: vehicle signature rejected"};
        case 6: return {true, true, false, true, "vehicle fault: vehicle counter rejected"};
        case 7: return {true, false, false, false, "vehicle fault: vehicle permission denied"};
        case 8: return {true, false, false, false, "vehicle fault: invalid vehicle domain"};
        case 9: return {true, false, false, false, "vehicle fault: invalid vehicle command"};
        case 10: return {true, false, false, false, "vehicle fault: vehicle decoding failed"};
        case 11: return {true, true, false, false, "vehicle fault: vehicle internal error"};
        case 12: return {true, false, false, false, "vehicle fault: vehicle VIN mismatch"};
        case 13: return {true, false, false, false, "vehicle fault: invalid vehicle parameter"};
        case 14: return {true, false, false, false, "vehicle fault: vehicle keychain full"};
        case 15: return {true, true, false, true, "vehicle fault: vehicle session epoch rejected"};
        case 16: return {true, false, false, false, "vehicle fault: invalid vehicle IV length"};
        case 17: return {true, true, false, true, "vehicle fault: vehicle command expired"};
        case 18: return {true, false, false, false, "vehicle fault: vehicle identity unavailable"};
        case 19: return {true, false, false, false, "vehicle fault: vehicle metadata hash failed"};
        case 20: return {true, true, false, true, "vehicle fault: vehicle command TTL rejected"};
        case 21: return {true, false, false, false, "vehicle fault: vehicle remote access disabled"};
        case 22: return {true, false, false, false, "vehicle fault: vehicle service access disabled"};
        case 23: return {true, false, false, false, "vehicle fault: vehicle account credentials required"};
        case 24: return {true, false, false, false, "vehicle fault: vehicle request MTU exceeded"};
        case 25: return {true, false, true, false, "vehicle fault: vehicle response MTU exceeded"};
        case 26: return {true, false, false, false, "vehicle fault: vehicle repeated counter"};
        case 27: return {true, false, false, false, "vehicle fault: invalid vehicle key handle"};
        case 28: return {true, false, false, false, "vehicle fault: vehicle response encryption required"};
        default: return {false, false, true, false, "vehicle fault: unknown vehicle fault"};
    }
}
inline constexpr bool fault_allows_retry(MessageFaultDecision decision, bool fault_authenticated,
                                         bool payload_maybe_sent, CommandEffect effect) noexcept {
    return decision.known && decision.temporary && !decision.possible_success &&
        (fault_authenticated || !payload_maybe_sent || effect == CommandEffect::ReadOnly);
}
inline constexpr CommandFailureInfo fault_failure_info(MessageFaultDecision decision,
        bool fault_authenticated, bool payload_maybe_sent) noexcept {
    return {decision.known ? (decision.temporary ? CommandError::Severity::Temporary
                                               : CommandError::Severity::Permanent)
                          : CommandError::Severity::Unknown,
            decision.possible_success || (payload_maybe_sent && !fault_authenticated)
                ? CommandError::Outcome::MayHaveSucceeded : CommandError::Outcome::DefinitelyFailed};
}
inline constexpr const char* uncertain_command_reason() noexcept {
    return "command outcome unknown; command may have executed";
}
}  // namespace tk
