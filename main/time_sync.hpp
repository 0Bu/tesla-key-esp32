#pragma once
#include <cstdint>
#include "logic/clock_policy.hpp"

// Only validated successful NTP application in this boot permits durable new dates.
// Restored and browser time are temporary estimates. Historical dates have no source marker.
bool clock_is_authoritative() noexcept;
bool clock_synced_via_ntp() noexcept; // ever committed; a later failed sync does not clear it
int64_t clock_plausibility_ceiling() noexcept; // exclusive, build year + 10

enum class BrowserClockStatus : uint8_t { Applied, AlreadyNtpSynced, Busy, Invalid, SetFailed };
struct BrowserClockResult { BrowserClockStatus status; int64_t seconds; };

// Serialized with the strong SNTP hook; never persists or marks browser time authoritative.
// Busy/Invalid/AlreadyNtpSynced prove no browser write; SetFailed preserves effect uncertainty.
BrowserClockResult apply_browser_clock(double milliseconds) noexcept;
BrowserClockResult apply_browser_clock_sample(tk::ClockSample sample) noexcept;
