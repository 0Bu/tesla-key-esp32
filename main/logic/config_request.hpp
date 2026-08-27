#pragma once

#include <string>

// Pure orchestration for a persisted one-string configuration request. The IDF/cJSON shell turns
// a body into ConfigStringSubmission; this function owns the ordering invariant: rejected input
// returns before load/probe/save/restart, response is sent before a successful restart, and an
// explicit empty string remains a valid disable operation.
namespace tk {

enum class ConfigSubmissionStatus {
    Ready,
    MissingBody,
    TooLarge,
    BodyNoMemory,
    ReceiveFailed,
    MalformedJson,
    JsonTooDeep,
    JsonUnsupportedNul,
    JsonNoMemory,
    ObjectRequired,
    StringFieldRequired,
};

struct ConfigStringSubmission {
    ConfigSubmissionStatus status{ConfigSubmissionStatus::MissingBody};
    std::string value;
};

inline constexpr int config_submission_http_status(ConfigSubmissionStatus status) {
    return status == ConfigSubmissionStatus::TooLarge ? 413
         : status == ConfigSubmissionStatus::BodyNoMemory ||
           status == ConfigSubmissionStatus::JsonNoMemory ? 503
         : status == ConfigSubmissionStatus::Ready ? 200
                                                    : 400;
}

inline constexpr const char* config_submission_reason(ConfigSubmissionStatus status) {
    return status == ConfigSubmissionStatus::MissingBody ? "missing body"
         : status == ConfigSubmissionStatus::TooLarge ? "request body too large"
         : status == ConfigSubmissionStatus::BodyNoMemory ||
           status == ConfigSubmissionStatus::JsonNoMemory ? "out of memory"
         : status == ConfigSubmissionStatus::ReceiveFailed ? "request body read failed"
         : status == ConfigSubmissionStatus::MalformedJson ? "invalid JSON"
         : status == ConfigSubmissionStatus::JsonTooDeep ? "JSON nesting too deep"
         : status == ConfigSubmissionStatus::JsonUnsupportedNul ? "JSON NUL escape not supported"
         : status == ConfigSubmissionStatus::ObjectRequired ? "JSON object required"
         : status == ConfigSubmissionStatus::StringFieldRequired ? "required string field missing"
                                                                  : "";
}

struct ConfigProbeVerdict {
    bool        ok{true};
    int         status{200};
    const char* reason{""};
};

struct ConfigStringMessages {
    const char* unchanged_empty;
    const char* unchanged_value;
    const char* invalid_value;
    const char* saved_empty;
    const char* saved_value;
    const char* save_failed;
};

template <typename Normalize, typename Load, typename Validate, typename Probe, typename Save,
          typename Respond, typename Commit>
int apply_config_string(const ConfigStringSubmission& submission,
                        Normalize normalize, Load load, Validate validate, Probe probe, Save save,
                        Respond respond, Commit commit, const ConfigStringMessages& messages) {
    if (submission.status != ConfigSubmissionStatus::Ready) {
        return respond(config_submission_http_status(submission.status), false,
                       config_submission_reason(submission.status));
    }

    const std::string value = normalize(submission.value);
    const std::string current = load();
    if (value == current) {
        return respond(200, true,
                       value.empty() ? messages.unchanged_empty : messages.unchanged_value);
    }
    if (!validate(value)) return respond(400, false, messages.invalid_value);

    const ConfigProbeVerdict checked = probe(value);
    if (!checked.ok) return respond(checked.status, false, checked.reason);

    const bool stored = save(value);
    const int response = respond(stored ? 200 : 500, stored,
        stored ? (value.empty() ? messages.saved_empty : messages.saved_value)
               : messages.save_failed);
    if (stored) commit();
    return response;
}

}  // namespace tk
