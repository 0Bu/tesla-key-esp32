#pragma once

// Returns true if the system clock has been authoritatively synchronized this boot,
// either via SNTP (on_time_sync) or via an explicit browser timestamp (apply_browser_clock).
// A wall clock restored from NVS (restore_clock_from_nvs) is NOT authoritative because it
// represents historical time from a prior shutdown/sync.
bool clock_is_authoritative() noexcept;

// Marks the system clock as authoritatively synchronized (called on SNTP sync or browser clock apply).
void mark_clock_authoritative() noexcept;

// True specifically if SNTP has synced this boot. Used by HTTP /set_time and /ota/check
// to avoid overwriting a verified NTP time with browser time.
bool clock_synced_via_ntp() noexcept;
