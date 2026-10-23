#pragma once

#include "reset_reason.hpp"
#include "coredump_image.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// Pure crash-report data and presentation. diag_crash.cpp owns flash, admission and
// the immutable boot capture. No renderer infers a summary from a nonzero PC/name.
namespace tk {
struct CrashFacts {
    int reset_code{0};
    bool fault{false}, coredump{false}, foreign_dump{false}, dismissed{false};
    bool artifact_present{false}, raw_available{false}, query_error{false}, checksum_verified{false};
    bool summary_available{false}, corrupted{false}, truncated{false}, isr{false};
    DumpIdentity dump_identity{DumpIdentity::Unverified};
};

template <class Text, class Frames> struct CrashData : CrashFacts {
    Text task;
    uint32_t pc{0};
    Frames backtrace;
    Text elf_sha256, dump_elf_sha256;
};
using CrashInfo = CrashData<std::string, std::vector<uint32_t>>;

// Borrow only immutable boot-owned payload. A view is metadata, never export authority.
class CrashTraceView {
public:
    CrashTraceView() = default;
    CrashTraceView(const uint32_t* data, size_t size) : data_(data && size ? data : &empty_), size_(data ? size : 0) {}
    size_t size() const noexcept { return size_; }
    const uint32_t& operator[](size_t index) const noexcept { return data_[index]; }
    const uint32_t* begin() const noexcept { return data_; }
    const uint32_t* end() const noexcept { return data_ + size_; }
private:
    inline static constexpr uint32_t empty_ = 0;
    const uint32_t* data_{&empty_};
    size_t size_{0};
};
using CrashInfoView = CrashData<std::string_view, CrashTraceView>;

inline CrashInfoView crash_borrow(const CrashInfo& owner) noexcept {
    CrashInfoView view;
    static_cast<CrashFacts&>(view) = owner;
    view.task = owner.task;
    view.pc = owner.pc;
    view.backtrace = {owner.backtrace.data(), owner.backtrace.size()};
    view.elf_sha256 = owner.elf_sha256;
    view.dump_elf_sha256 = owner.dump_elf_sha256;
    return view;
}

// The owner must outlive the view and must not mutate/reallocate its payload.
CrashInfoView crash_borrow(const CrashInfo&&) = delete;

struct CrashSnapshotState {
    uint32_t before, after;
    bool matching_header, query_error, readable, current, foreign, absent;
};
// Called only after building the borrowed payload and loading the final generation.
// A revoked view can still name the running build, but can never grant dump authority.
inline void crash_finish_snapshot(CrashInfoView& info, const CrashSnapshotState& state) noexcept {
    const bool stable = state.before == state.after && !state.query_error && state.matching_header;
    info.coredump = stable && state.current && state.readable;
    if (!stable || (!state.current && !state.foreign)) {
        info.dump_identity = DumpIdentity::Unverified; info.checksum_verified = false;
        info.foreign_dump = false; info.dump_elf_sha256 = {};
    }
    if (!info.coredump) {
        info.summary_available = info.corrupted = info.truncated = info.isr = false;
        info.task = {}; info.pc = 0; info.backtrace = {};
    }
    if (state.before != state.after) info.raw_available = false;
    if (state.absent) {
        info.artifact_present = info.raw_available = false;
        info.dump_identity = DumpIdentity::Unverified; info.checksum_verified = false;
        info.foreign_dump = false; info.dump_elf_sha256 = {};
    }
}

inline void crash_set_reset(CrashFacts& c, int code) {
    c.reset_code = code;
    c.fault = reset_is_fault(code);
}
inline bool crash_has_summary(const CrashFacts& c) { return c.summary_available; }
inline bool crash_is_notable(const CrashFacts& c) {
    return !c.dismissed && (reset_is_fault(c.reset_code) || c.coredump ||
           c.artifact_present || c.foreign_dump || c.query_error);
}

// Detection never authorizes erasing an artifact. Kept for pure identity callers;
// the boot parser separately requires a checked checksum and full supported metadata.
inline bool coredump_is_foreign(const std::string& running, const std::string& dump) {
    return classify_dump_identity(running.data(), running.size(), dump.data(), dump.size(), true)
           == DumpIdentity::Foreign;
}

enum class CrashEraseResult { Busy, Absent, Erased, Unconfirmed };
inline constexpr bool crash_erase_permits_dismiss(CrashEraseResult result) {
    return result == CrashEraseResult::Absent || result == CrashEraseResult::Erased;
}
inline constexpr size_t kCrashBacktraceMax = 16;
inline void append_hex32(std::string& out, uint32_t value) {
    char text[11];
    std::snprintf(text, sizeof(text), "0x%08x", static_cast<unsigned>(value));
    out += text;
}
template <class Crash> inline std::string build_crash_text(const Crash& c) {
    std::string text = "reset=";
    text += reset_reason_slug(c.reset_code);
    text += reset_is_fault(c.reset_code) ? "  fault=yes" : "  fault=no";
    text += c.coredump ? "  coredump=yes" : "  coredump=no";
    text += c.artifact_present ? "  artifact=yes" : "  artifact=no";
    text += c.raw_available ? "  raw=yes" : "  raw=no";
    text += "  identity=";
    text += dump_identity_name(c.dump_identity);
    if (c.query_error) text += "  query_error=yes";
    if (crash_has_summary(c)) {
        text += "\ntask=";
        text += c.task.empty() ? "?" : c.task;
        text += "  pc=";
        append_hex32(text, c.pc);
        text += c.isr ? "  isr=yes" : "  isr=no";
        text += "\nbacktrace:";
        const size_t depth = c.backtrace.size() < kCrashBacktraceMax
                           ? c.backtrace.size() : kCrashBacktraceMax;
        for (size_t i = 0; i < depth; ++i) { text += ' '; append_hex32(text, c.backtrace[i]); }
        if (c.truncated || depth < c.backtrace.size()) text += "  (truncated)";
        if (c.corrupted) text += "  (corrupted)";
    }
    if (!c.elf_sha256.empty()) { text += "\nelf_sha256="; text += c.elf_sha256; }
    if (!c.dump_elf_sha256.empty() && c.dump_elf_sha256 != c.elf_sha256) {
        text += "\ndump_elf_sha256="; text += c.dump_elf_sha256;
    }
    return text;
}
}  // namespace tk
