#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

// Clock admission and persisted-date decisions only. main.cpp owns clock serialization and NVS;
// HTTP glue owns replies. A plausible value is not proof of its source or of physical time.
namespace tk {

inline constexpr int64_t kClockEpochFloor = 1700000000;
inline constexpr int64_t kHistoricalDateFloor = 1600000000;

constexpr int64_t clock_jan1_seconds(int year) {
    if (year < 1970 || year > 10009) return 0;
    const int64_t days = 365LL * (year - 1970) + (year - 1969) / 4
                      - (year - 1901) / 100 + (year - 1601) / 400;
    return days * 86400;
}

inline int64_t clock_ceiling_seconds(std::string_view compiler_date) {
    int year = 0;
    if (compiler_date.size() >= 4) {
        for (char digit : compiler_date.substr(compiler_date.size() - 4)) {
            if (digit < '0' || digit > '9') { year = 0; break; }
            year = year * 10 + digit - '0';
        }
    }
    if (year < 2023) year = 2023; // same conservative fallback as the existing browser policy
    return clock_jan1_seconds(year + 10);
}

struct ClockSample {
    int64_t seconds{0};
    int32_t microseconds{0};
};

constexpr bool clock_ceiling_valid(int64_t ceiling) {
    return ceiling > kClockEpochFloor && ceiling <= clock_jan1_seconds(10009);
}

constexpr bool clock_sample_valid(ClockSample sample, int64_t ceiling) {
    return clock_ceiling_valid(ceiling) && sample.seconds >= kClockEpochFloor &&
           sample.seconds < ceiling && sample.microseconds >= 0 && sample.microseconds < 1000000;
}

constexpr bool historical_date_valid(int64_t seconds, int64_t ceiling) {
    return clock_ceiling_valid(ceiling) && seconds > kHistoricalDateFloor && seconds < ceiling;
}

inline bool clock_parse_decimal(std::string_view text, uint64_t& out) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const unsigned digit = static_cast<unsigned>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

inline bool clock_parse_checkpoint(std::string_view text, int64_t ceiling, int64_t& out) {
    uint64_t value = 0;
    if (!clock_parse_decimal(text, value) || value > uint64_t(INT64_MAX) ||
        !clock_sample_valid({static_cast<int64_t>(value), 0}, ceiling)) return false;
    out = static_cast<int64_t>(value);
    return true;
}

inline bool clock_parse_date(std::string_view text, int64_t ceiling, int64_t& out) {
    uint64_t value = 0;
    if (!clock_parse_decimal(text, value) || value > uint64_t(INT64_MAX) ||
        !historical_date_valid(static_cast<int64_t>(value), ceiling)) return false;
    out = static_cast<int64_t>(value);
    return true;
}

inline bool browser_clock_sample(double milliseconds, int64_t ceiling, ClockSample& out) {
    if (!clock_ceiling_valid(ceiling) || !std::isfinite(milliseconds) ||
        milliseconds < double(kClockEpochFloor) * 1000 || milliseconds >= double(ceiling) * 1000)
        return false;
    // The bounded four-digit build year keeps this conversion well inside int64_t and the
    // exact-integer double range. Preserve the old whole-millisecond browser precision.
    const auto whole_ms = static_cast<int64_t>(milliseconds);
    const ClockSample value{whole_ms / 1000, static_cast<int32_t>(whole_ms % 1000) * 1000};
    if (!clock_sample_valid(value, ceiling)) return false;
    out = value;
    return true;
}

inline bool clock_parse_query_ms(std::string_view text, int64_t ceiling, ClockSample& out) {
    uint64_t value = 0;
    if (!clock_ceiling_valid(ceiling) || !clock_parse_decimal(text, value) ||
        value < uint64_t(kClockEpochFloor) * 1000 || value >= uint64_t(ceiling) * 1000)
        return false;
    out = {static_cast<int64_t>(value / 1000), static_cast<int32_t>(value % 1000) * 1000};
    return true;
}

constexpr bool clock_should_restore(int64_t current_seconds) {
    return current_seconds < kClockEpochFloor;
}

inline bool clock_advance_sample(ClockSample sample, int64_t elapsed_us, int64_t ceiling,
                                 ClockSample& out) {
    if (!clock_sample_valid(sample, ceiling) || elapsed_us < 0) return false;
    const int64_t add_seconds = elapsed_us / 1000000;
    const int32_t micros = sample.microseconds + static_cast<int32_t>(elapsed_us % 1000000);
    const int64_t carry = micros / 1000000;
    if (add_seconds >= ceiling - sample.seconds || carry >= ceiling - sample.seconds - add_seconds)
        return false;
    out = {sample.seconds + add_seconds + carry, micros % 1000000};
    return true;
}

struct ClockCheckpointPolicy {
    static constexpr uint64_t kSuccessIntervalUs = 6ULL * 60 * 60 * 1000000;
    static constexpr uint64_t kAttemptIntervalUs = 5ULL * 60 * 1000000;
    uint64_t last_attempt_us{0}, last_success_us{0};
    bool attempted{false}, succeeded{false};

    bool due(uint64_t now_us) const {
        if (attempted && now_us - last_attempt_us < kAttemptIntervalUs) return false;
        return !succeeded || now_us - last_success_us >= kSuccessIntervalUs;
    }
    void note_attempt(uint64_t now_us) { attempted = true; last_attempt_us = now_us; }
    void note_result(bool success, uint64_t now_us) {
        if (success) { succeeded = true; last_success_us = now_us; }
    }
};

} // namespace tk
