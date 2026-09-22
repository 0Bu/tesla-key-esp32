#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// Pure, hardware-free command-outcome text, shared by the REST /command reason
// (http_api.cpp) and the MCP tools/call result (mcp_server.cpp) so the two paths can
// never report the same outcome differently: success → fixed string; failure with a
// reply → the car's own reason; no reply → "vehicle not reachable".
//
// LIFETIME: on the failure-with-reason path the returned pointer aliases `err` — the
// caller must keep `err` alive (a named local, not a temporary bound across statements)
// until the text has been consumed/copied.
namespace tk {

// Evaluates whether an error string represents a nominal already_set response from the vehicle
// (teslamotors/vehicle-command NominalError, treated as idempotent success by caller).
inline bool is_nominal_already_set(std::string_view err) noexcept {
    return err == "already_set";
}

inline const char* command_result_text(bool ok, const std::string& err) {
    if (ok || is_nominal_already_set(err)) return "command executed successfully";
    return err.empty() ? "vehicle not reachable" : err.c_str();
}

// Where a failed command's outcome came from, for the soft-desync link backstop:
// - VehicleResponse: the car answered (auth/role refusal, whitelist status, rejection, VCSEC error).
//   The link is demonstrably working, so it resets the failure streak and counts as contact.
// - LocalPolicy: decided on the device without any BLE exchange (the command runner's
//   "vehicle asleep" skip/fail from the cached VCSEC flag). Neither a transport fault nor
//   proof of contact.
// - TransportOrTimeout: everything else (timeouts, write/build failures, lost link); counts
//   toward the drop-and-resync streak.
enum class CommandFailureOrigin : uint8_t {
    TransportOrTimeout,
    VehicleResponse,
    LocalPolicy,
};

inline CommandFailureOrigin classify_command_failure(std::string_view err) noexcept {
    if (err.find("vehicle asleep") != std::string_view::npos) return CommandFailureOrigin::LocalPolicy;
    if (err.find("authentication failed") != std::string_view::npos ||
        err.find("whitelist") != std::string_view::npos ||
        err.find("rejected") != std::string_view::npos ||
        err.find("action failed") != std::string_view::npos ||
        err.find("failed with error status") != std::string_view::npos ||
        err.find("VCSEC command failed") != std::string_view::npos) {
        return CommandFailureOrigin::VehicleResponse;
    }
    return CommandFailureOrigin::TransportOrTimeout;
}

class CommandError {
public:
    enum class Severity {
        Temporary,
        Permanent,
        Unknown
    };

    enum class Outcome {
        MayHaveSucceeded,
        DefinitelyFailed,
        Unknown
    };

    explicit CommandError(std::string msg,
                          Severity severity = Severity::Temporary,
                          Outcome outcome = Outcome::DefinitelyFailed)
        : msg_(std::move(msg)), severity_(severity), outcome_(outcome) {}

    const std::string& message() const noexcept { return msg_; }
    Severity severity() const noexcept { return severity_; }
    Outcome outcome() const noexcept { return outcome_; }

private:
    std::string msg_;
    Severity severity_;
    Outcome outcome_;
};

class OperationResult {
public:
    static OperationResult success() {
        return OperationResult(true, nullptr);
    }

    static OperationResult failure(std::unique_ptr<CommandError> error) {
        return OperationResult(false, std::move(error));
    }

    static OperationResult failure(std::string msg) {
        return OperationResult(false, std::make_unique<CommandError>(std::move(msg)));
    }

    bool compatible_success() const noexcept { return success_; }
    bool is_success() const noexcept { return success_; }
    bool is_failure() const noexcept { return !success_; }
    const CommandError* error() const noexcept { return error_.get(); }
    std::unique_ptr<CommandError> release_error() noexcept { return std::move(error_); }

    OperationResult(OperationResult&&) noexcept = default;
    OperationResult& operator=(OperationResult&&) noexcept = default;
    OperationResult(const OperationResult&) = delete;
    OperationResult& operator=(const OperationResult&) = delete;

private:
    OperationResult(bool ok, std::unique_ptr<CommandError> err)
        : success_(ok), error_(std::move(err)) {}

    bool success_{false};
    std::unique_ptr<CommandError> error_;
};

}  // namespace tk
