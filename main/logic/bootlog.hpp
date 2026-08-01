#pragma once
// The one-shot records a log forwarder replays once per boot — build identity, and the crash the
// device woke up from. Pure, IDF-free, host-testable.
//
// WHY THIS EXISTS. Two independent facts make the most valuable lines of a boot the ones least
// likely to be readable:
//
//   * TIMING. Crash capture has to run at the very top of app_main, before NVS, before WiFi and
//     before the syslog forwarder's queue exists — the values must be sampled before anything
//     else has allocated or rebooted. But a line logged before the forwarder starts has nowhere
//     to go: the capture hook drops it, and the only copy lands in the in-RAM diag ring. The ring
//     is small and the next restart erases it, so on a device that reboots again — which is what
//     a crash-shaped failure does — the crash is readable NOWHERE. Syslog is the only record that
//     survives a restart, which is precisely why the reset-reason line in this firmware is
//     emitted AFTER the forwarder starts rather than where its values are read. These records are
//     the same idea generalized: build them at boot, hand them to the forwarder the moment it has
//     a collector, once.
//
//   * SIZE. The paste-friendly block from logic/crashinfo.hpp (build_crash_text) is multi-line
//     and, worst case, longer than one log line: this firmware's diag and syslog paths both carry
//     a 256-byte line buffer, so a long crash block truncates — and it truncates at the END,
//     which is exactly where the backtrace and the ELF hash live. A record that drops the two
//     fields it exists to carry is worse than no record, because it still looks like one. So
//     these are SINGLE-LINE records, each one datagram, each bounded by construction (see
//     kBootlogLineMax below) rather than by hoping the inputs stay short.
//
// build_crash_text() is untouched by all this: it stays the human-facing block for /diag and a
// copy-paste bundle, where multi-line is a feature and nothing truncates it.
//
// FORMAT is logfmt-ish — a leading record tag, then key=value pairs separated by single spaces —
// so a collector can extract fields without a bespoke parser, and a human can read it without
// one. The tags (BOOT / CRASH) match the vocabulary this firmware's boot lines already use.
//
// A NOTE ON WHO CALLS THESE. Emitting them must not be able to take the device down. Both
// builders allocate (std::string, and the caller's vector), and a forwarder task is a C frame
// boundary — an escaping std::bad_alloc reaches std::terminate and reboots. Since the replay runs
// on EVERY boot, that would be a boot LOOP rather than a one-off crash, so the caller wraps the
// replay in try/catch and treats an out-of-memory replay as done rather than retrying forever: a
// device that cannot spare a few hundred bytes has a worse problem than a missing log line.
#include <cstddef>
#include <string>
#include <vector>

#include "crashinfo.hpp"

namespace tk {

// Hard ceiling on every record built here. The diag ring and the syslog forwarder in this
// firmware both format into a 256-byte line buffer, and a UDP datagram is written per line, so
// anything at or under this figure survives whole on both paths with room to spare for whatever
// prefix a transport adds (an RFC 5424 header, a timestamp). It is a BUDGET, not a wish — every
// variable-length field below is capped and the arithmetic is stated at each builder, so no input
// can push a record past it.
inline constexpr size_t kBootlogLineMax = 200;

// Per-field caps, chosen so the worst-case line still fits the budget above. A truncated value is
// still a usable one (a hash prefix identifies a build, a clipped task name still names a task);
// a truncated LINE is not, because the transport clips whichever field happened to be last.
inline constexpr size_t kBootlogVersionMax = 48;   // a semver + pre-release tag, with slack
inline constexpr size_t kBootlogShaMax     = 64;   // a full SHA-256 in hex; usually far shorter
inline constexpr size_t kBootlogTaskMax    = 32;   // FreeRTOS names are ~16; the cap is the guard

// Deepest backtrace a record carries is logic/crashinfo.hpp's kCrashBacktraceMax — the same bound
// the human-facing bundle uses, deliberately not a second constant here: the log stream and the
// pasted report showing different amounts of one stack is a discrepancy a reader has to spend
// time discovering is not a difference between two crashes.

// Upper bound on the records build_crash_log_lines() appends, so a caller can reserve.
inline constexpr int kCrashLogLineMax = 3;

namespace detail {

// Append at most `max` characters, substituting "?" for an absent or empty value. The "?" matters:
// an empty value renders as a dangling "version=" that a logfmt parser may read as a missing KEY
// rather than a missing value, and a reader cannot tell "the device did not report this" from "a
// field was added and this build predates it".
inline void append_capped(std::string& out, const char* v, size_t max) {
    if (v == nullptr || *v == '\0') {
        out += '?';
        return;
    }
    size_t n = 0;
    while (n < max && v[n] != '\0') n++;
    out.append(v, n);
}

inline void append_capped(std::string& out, const std::string& v, size_t max) {
    if (v.empty()) {
        out += '?';
        return;
    }
    out.append(v, 0, max);
}

}  // namespace detail

// The build-identity record, emitted on EVERY boot including the entirely unremarkable ones.
//
// Without it a log stream cannot be tied to a binary. A running device can be asked what it is,
// but a stream outlives the build that wrote it, and the question a post-mortem asks first — "is
// this the version that has the fix?" — has no other answer in the log. `elf` is the same app-ELF
// hash a core dump reports, so a dump, a report and a stream can be matched to one binary; the
// reset slug says how this boot started (logic/reset_reason.hpp — ONE vocabulary, so this line
// and a crash record cannot name the same event differently).
//
// `safe_mode` is a latched degraded/recovery-boot flag: whether the device came up deliberately
// reduced rather than normally. Pass false when the firmware has no such state. It rides on this
// line rather than a line of its own because a reader needs it in the same record as the version
// — "which firmware, started how" is one question.
//
// Worst case: 13 + 48 + 5 + 64 + 7 + 16 + 13 = 166 chars, inside kBootlogLineMax.
inline std::string build_boot_line(const char* version, const char* elf_sha256,
                                   const char* reset_slug, bool safe_mode) {
    std::string s = "BOOT version=";
    detail::append_capped(s, version, kBootlogVersionMax);
    s += " elf=";
    detail::append_capped(s, elf_sha256, kBootlogShaMax);
    s += " reset=";
    detail::append_capped(s, reset_slug, 16);
    s += safe_mode ? " safe_mode=yes" : " safe_mode=no";
    return s;
}

// Append the crash as up to kCrashLogLineMax single-line records; returns how many were added.
//
// Returns 0 — appending NOTHING — for a boot that is not notable. That rule lives here, where it
// is host-tested, rather than at the call site: a forwarder that replays a "crash" record on every
// ordinary power-on teaches its reader to skip crash records, and the day a real one appears it is
// skipped too. crash_is_notable() is the single definition of what is worth saying, shared with
// every other consumer of a CrashInfo, so the log stream and the device's own report cannot
// disagree about whether something happened.
//
// Records, in the order they are appended (each is self-contained — a lost datagram costs one
// record, not the meaning of the others):
//   1. reset / fault / dump present  — always, and the only record for a dump with no summary
//   2. task / pc / corrupted / hash  — the identity of the crash
//   3. backtrace                     — raw PCs, symbolized offline against the matching .elf
//
// Truncation, when it is needed at all, takes the BACKTRACE and never the hash: without the hash
// the PCs cannot be symbolized against anything, so they would be a column of numbers nobody can
// resolve — the two fields are not equally valuable, and the tail frames are the cheap half.
//
// Allocates (the strings, and the vector's growth). See the note at the top of this header about
// guarding the caller.
inline int build_crash_log_lines(const CrashInfo& c, std::vector<std::string>& out) {
    if (!crash_is_notable(c)) return 0;
    int n = 0;

    // 1. Worst case: 12 + 16 + 10 + 13 + 17 + 64 = 132 chars.
    std::string head = "CRASH reset=";
    detail::append_capped(head, reset_reason_slug(c.reset_code), 16);
    head += reset_is_fault(c.reset_code) ? " fault=yes" : " fault=no";
    head += c.coredump ? " coredump=yes" : " coredump=no";
    // Only when the dump names a DIFFERENT build than the running one: that is the state in which
    // the dump cannot be symbolized against this firmware, and saying so beside coredump=yes is
    // the difference between a reader chasing a decode failure and understanding it. Identical
    // hashes are the normal case and are already on the BOOT line.
    if (!c.dump_elf_sha256.empty() && c.dump_elf_sha256 != c.elf_sha256) {
        head += " dump_elf_sha256=";
        detail::append_capped(head, c.dump_elf_sha256, kBootlogShaMax);
    }
    out.push_back(head);
    n++;

    // An orphan dump, or a summary that could not be parsed: the header is all there is, and
    // saying that much is still worth a datagram — "a dump is waiting" is actionable on its own.
    if (!crash_has_summary(c)) return n;

    // 2. Worst case: 11 + 32 + 14 + 14 + 12 + 64 = 147 chars.
    std::string sum = "CRASH task=";
    detail::append_capped(sum, c.task, kBootlogTaskMax);
    sum += " pc=";
    append_hex32(sum, c.pc);
    if (c.corrupted) sum += " corrupted=yes";
    if (!c.elf_sha256.empty()) {
        sum += " elf_sha256=";
        detail::append_capped(sum, c.elf_sha256, kBootlogShaMax);
    }
    out.push_back(sum);
    n++;

    // 3. "CRASH backtrace=" is 16 chars and each PC costs 11 (a separator plus 10 fixed hex
    //    characters), so kCrashBacktraceMax = 16 frames land at 191 — inside the budget. The loop
    //    still checks the budget per frame rather than trusting that arithmetic: the moment the
    //    prefix or the hex width changes, a silent overflow would truncate this line at the
    //    transport instead of here, and a clipped hex PC reads as a different, valid-looking
    //    address.
    const size_t depth =
        c.backtrace.size() < kCrashBacktraceMax ? c.backtrace.size() : kCrashBacktraceMax;
    if (depth > 0) {
        std::string bt = "CRASH backtrace=";
        for (size_t i = 0; i < depth; i++) {
            if (bt.size() + 11 > kBootlogLineMax) break;
            if (i) bt += ' ';
            append_hex32(bt, c.backtrace[i]);
        }
        out.push_back(bt);
        n++;
    }
    return n;
}

}  // namespace tk
