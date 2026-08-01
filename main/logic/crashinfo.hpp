#pragma once
// What the device knows about the boot it just made, and the three judgements that depend on it:
// is this worth REPORTING, does the core dump in flash even belong to the RUNNING build, and how
// does a human read it. Pure, IDF-free, host-testable.
//
// WHY THIS EXISTS. A crash on this board leaves two artefacts, and neither speaks for itself.
// The reset reason is one integer, available on every boot including the ordinary ones. The core
// dump is a blob in its own flash partition whose SUMMARY (crashed task, exception PC, backtrace,
// the app-ELF hash of the build that wrote it) can be parsed once at boot. Turning that pair into
// "a crash happened, here is what it was" is a set of small decisions that each fail quietly:
//
//   * Report EVERY boot and the report means nothing. A USB re-enumeration, an OTA reboot and the
//     heap watchdog's deliberate restart all look like a reset; if each raises a crash record the
//     reader learns to skip crash records. crash_is_notable() is the gate, and it is deliberately
//     narrow: a real fault, or a dump for THIS build still sitting in flash.
//   * Report a FOREIGN dump and you hand out an artefact the decoder will refuse. The coredump
//     partition survives an OTA, and a panic that cannot write its own dump (a stack overflow can
//     overrun the writer) leaves the PREVIOUS build's dump in place — a structurally valid image
//     of a different binary. It passes every "is there a dump?" check, so the device offers it,
//     and only the offline symbolizer three steps later rejects it on a hash mismatch, by which
//     time the reader believes they are looking at the crash they are investigating.
//     coredump_is_foreign() is what spots that, and it is conservative for a hard reason: the
//     caller ERASES on true, and erasing a dump that really is ours destroys the only evidence a
//     panic left behind. So it answers true ONLY on proof — two present hashes, a meaningful
//     common prefix, and an actual mismatch inside it.
//
// The backtrace stays RAW program counters. Nothing here reads an ELF or symbolizes anything;
// the PCs are decoded offline against the matching build. That is also why the running build's
// elf_sha256 travels with the record: it is what ties a log stream, a report and a .elf together.
//
// NOT A JSON BUILDER — deliberately, and please do not re-add one. This firmware shapes its JSON
// in ONE place (logic/status_model.hpp, whose field contract is golden-pinned by host tests) and
// serializes it through that emitter. A second builder here would be a second, unpinned copy of
// part of the /status contract, free to disagree about a key name or a presence rule with nothing
// checking. This header supplies the DATA plus the PREDICATES; the emitter decides how it is
// rendered as JSON. build_crash_text() below is the one rendering that lives here, because a
// paste-friendly block for a human is not a wire contract.
//
// The design is ported from a sibling ESP-IDF firmware where it has been carrying real crash
// reports for a while; the rules below (especially the conservatism of coredump_is_foreign) are
// its rules, not new inventions.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "reset_reason.hpp"   // ONE reset vocabulary — slug + fault classification

namespace tk {

// Everything captured about the last reset, filled ONCE at boot from esp_reset_reason() plus (if
// present and valid) esp_core_dump_get_summary(). Never re-parsed on a request path: parsing the
// summary costs a multi-KB allocation, and doing it per request on a heap whose binding limit is
// the largest contiguous block is exactly the kind of thing that turns a diagnostic into an
// outage. The one field worth re-reading live is `coredump` (a 4-byte flash size-word read),
// because the image can be erased mid-session and a cached true would strand a banner offering a
// download that 404s.
struct CrashInfo {
    // Raw esp_reset_reason() value; spelled by reset_reason_slug(), classified by reset_is_fault().
    int  reset_code{0};
    // The CACHED verdict of reset_is_fault(reset_code) — carried so a consumer that only has the
    // struct does not have to re-derive it. Fill it through crash_set_reset() so the two cannot
    // drift; the predicates below deliberately consult reset_is_fault() rather than this field
    // (see crash_is_notable).
    bool fault{false};
    // A dump for THIS build is present in flash and downloadable. False after an orphan dump has
    // been erased (see coredump_is_foreign) — so it means "there is something the decoder will
    // accept", not merely "the partition is non-blank".
    bool coredump{false};
    // The unwinder flagged the backtrace as unreliable. Kept rather than dropped: a partial
    // backtrace still names the region, and a reader must be told which one they are holding.
    bool corrupted{false};
    // The user acknowledged and DELETED this report. A real deletion, not a per-browser "hide":
    // the caller erases the dump FIRST and only then sets this, so `dismissed` can never claim a
    // crash is dealt with while its evidence is still downloadable. RAM-only by design — after
    // any reboot the reset reason is no longer a fault and the dump is gone, while a NEW crash
    // captures a fresh CrashInfo and must show.
    bool dismissed{false};

    // Core-dump summary. Empty/zero when no summary was parsed (no dump, an invalid one, or an
    // orphan that was erased). See crash_has_summary().
    std::string           task;         // crashed task name
    uint32_t              pc{0};        // exception program counter
    std::vector<uint32_t> backtrace;    // raw PCs, symbolized offline against the matching .elf

    std::string elf_sha256;        // the RUNNING build's app-ELF hash (hex, possibly truncated)
    std::string dump_elf_sha256;   // the hash the DUMP reports — the two are compared, not assumed
};

// Set the reset code and its cached classification together. The struct carries `fault` as data
// but reset_is_fault() is the authority; going through here is what keeps a hand-filled CrashInfo
// from reporting reason="panic" beside fault=false.
inline void crash_set_reset(CrashInfo& c, int code) {
    c.reset_code = code;
    c.fault      = reset_is_fault(code);
}

// Was a core-dump summary parsed? DERIVED from the fields rather than carried as a separate flag,
// because a flag and the fields it describes can disagree and then the renderer prints an empty
// "task=  pc=0x00000000" block that reads like a crash with no information instead of like no
// summary. A summary in which the task is unnamed, the PC is zero AND the backtrace is empty
// carries nothing anyway, so the two states are worth conflating.
inline bool crash_has_summary(const CrashInfo& c) {
    return !c.task.empty() || c.pc != 0 || !c.backtrace.empty();
}

// Minimum hex-char overlap two app-ELF hashes must share before a DIFFERENCE between them counts
// as proof of a different build. Both sides may be truncated (ESP-IDF's own retrieve length is 9
// hex chars by default), so the shorter length governs; 8 hex chars is 32 bits, under that
// default, which means the ordinary equal-length case is compared in full while a pathologically
// short value can never make two renderings of the SAME hash look different by accident.
inline constexpr size_t kElfShaMinCompare = 8;

// Does the dump in flash belong to a DIFFERENT firmware than the one running now? Answer true
// ONLY on proof: both hashes present, a meaningful common prefix to compare, and a mismatch
// inside it. Everything else — either side empty, or a prefix too short to be evidence — is
// false, and that asymmetry is the whole point: the caller erases on true, so a false positive
// destroys the one artefact a panic left, while a false negative merely leaves a stale dump to be
// noticed later. A missing hash is not evidence of foreign origin, it is absence of evidence.
//
// Different truncation lengths are fine: two renderings of one hash agree on their common prefix.
inline bool coredump_is_foreign(const std::string& running_sha, const std::string& dump_sha) {
    if (running_sha.empty() || dump_sha.empty()) return false;
    const size_t n = running_sha.size() < dump_sha.size() ? running_sha.size() : dump_sha.size();
    if (n < kElfShaMinCompare) return false;
    return running_sha.compare(0, n, dump_sha, 0, n) != 0;
}

// Is this boot worth REPORTING at all? True for a real fault, or for a dump belonging to this
// build still waiting in flash (a fault that rebooted a second time before anyone collected it
// would otherwise report nothing at all) — and false once the user has dismissed it.
//
// Note it consults reset_is_fault(c.reset_code) rather than c.fault. The gate that decides
// whether anything is reported must not depend on a caller having remembered to fill a
// convenience field; a CrashInfo built by hand with reset_code set and fault left default would
// otherwise silently report nothing on a genuine panic — a diagnostic failing closed in the one
// direction it must not.
// ESP_ERR_NOT_FOUND, mirrored BY VALUE so this header stays IDF-free; diag_crash.cpp
// static_asserts it against the real macro, the same way reset_reason.hpp mirrors the reset enum.
inline constexpr int kEspErrNotFound = 0x105;

// Does a core-dump erase result still permit the crash report to be DISMISSED?
//
// A dismissal has two jobs — destroy the downloadable dump, and clear the report — and they do
// not always both apply. On a device whose INSTALLED partition table has no `coredump` partition
// the erase has nothing to destroy and returns ESP_ERR_NOT_FOUND. That is not an exotic corner:
// it is EVERY device upgraded by OTA, because a partition table is not part of an OTA image, and
// the project documents that as a supported state. Treating it as a failure made POST
// /crash/dismiss answer 500 there forever, so a fault-reset banner could never be acknowledged —
// precisely the outcome diag_crash.cpp's own comment says must not happen ("Failing it would
// leave a crash banner that no action can dismiss"). Confirmed on a live board: after the
// 1.4.64→1.4.66 OTA, dismiss returned 500 with `coredump erase: ESP_ERR_NOT_FOUND`.
//
// Every OTHER error still blocks, and that asymmetry is the point: any other failure means a dump
// may STILL be downloadable, and marking the report dismissed would assert the opposite. The
// caller's erase-first-mark-second ordering only means something while this stays narrow.
inline constexpr bool crash_erase_permits_dismiss(int err) {
    return err == 0 /* ESP_OK */ || err == kEspErrNotFound;
}

inline bool crash_is_notable(const CrashInfo& c) {
    if (c.dismissed) return false;
    return c.coredump || reset_is_fault(c.reset_code);
}

// Deepest backtrace any rendering carries. Defined here, once, and reused by the log-stream
// records in logic/bootlog.hpp so the two cannot disagree about how much of a crash gets told.
// The bound is not cosmetic: a summary can over-report its own depth, `backtrace` is a vector
// with no structural ceiling, and this firmware's diag ring formats one line into a 256-byte
// buffer — an unbounded PC list is clipped by the transport, at the end, where a half-written
// hex address reads as a different, entirely valid-looking one. Sixteen frames is past where a
// crash stops being identifiable by its stack.
inline constexpr size_t kCrashBacktraceMax = 16;

// "0x%08x" of one program counter. Fixed width on purpose: a column of equal-length PCs is
// readable at a glance and greppable, and the fixed 10 chars are what make the line-length
// budget in logic/bootlog.hpp arithmetic rather than a hope.
inline void append_hex32(std::string& out, uint32_t v) {
    char b[11];
    std::snprintf(b, sizeof(b), "0x%08x", static_cast<unsigned>(v));
    out += b;
}

// Paste-friendly multi-line rendering for a HUMAN — the diag ring at boot, and any "copy the
// diagnostics" bundle. Multi-line is a feature here (nothing truncates it, and a reader wants the
// backtrace on its own line); it is precisely why the log-stream records are a separate builder
// in logic/bootlog.hpp, where a datagram-sized single line is the requirement instead.
//
// Always states the reset and whether a dump is downloadable, even on an unremarkable boot — the
// caller decides whether to print it at all (crash_is_notable), and a renderer that silently
// produced nothing for some inputs would be indistinguishable from a renderer that failed.
inline std::string build_crash_text(const CrashInfo& c) {
    std::string t = "reset=";
    t += reset_reason_slug(c.reset_code);
    t += reset_is_fault(c.reset_code) ? "  fault=yes" : "  fault=no";
    t += c.coredump ? "  coredump=yes" : "  coredump=no";

    if (crash_has_summary(c)) {
        t += "\ntask=";
        t += c.task.empty() ? "?" : c.task;
        t += "  pc=";
        append_hex32(t, c.pc);
        t += "\nbacktrace:";
        const size_t depth =
            c.backtrace.size() < kCrashBacktraceMax ? c.backtrace.size() : kCrashBacktraceMax;
        for (size_t i = 0; i < depth; i++) {
            t += ' ';
            append_hex32(t, c.backtrace[i]);
        }
        // Say that frames were dropped rather than letting the list simply end: a backtrace that
        // stops is read as a stack that ended, i.e. as the bottom of the call chain.
        if (depth < c.backtrace.size()) t += " …";
        // Stated on the backtrace line itself: a reader who scrolls past a separate "corrupted"
        // field will otherwise trust PCs the unwinder did not.
        if (c.corrupted) t += "  (corrupted)";
    }

    if (!c.elf_sha256.empty()) {
        t += "\nelf_sha256=";
        t += c.elf_sha256;
    }
    // Only worth printing when it can say something the running hash does not: identical hashes
    // are the normal case and repeating them invites the reader to compare two identical strings
    // for a difference that is not there.
    if (!c.dump_elf_sha256.empty() && c.dump_elf_sha256 != c.elf_sha256) {
        t += "\ndump_elf_sha256=";
        t += c.dump_elf_sha256;
    }
    return t;
}

}  // namespace tk
