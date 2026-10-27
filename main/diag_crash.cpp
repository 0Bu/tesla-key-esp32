#include "diag_crash.hpp"
#include "coredump_hash.hpp"
#include "logic/bootlog.hpp"
#include <sdkconfig.h>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#if defined(CONFIG_IDF_TARGET_ARCH_XTENSA)
#include <xtensa_context.h>
#else
#include <riscv/rvruntime-frames.h>
#endif

namespace tk {
static const char* TAG = "diag_crash";
static_assert(static_cast<int>(ResetCode::Unknown) == ESP_RST_UNKNOWN, "reset enum drift");
static_assert(static_cast<int>(ResetCode::PowerOn) == ESP_RST_POWERON, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Ext) == ESP_RST_EXT, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Sw) == ESP_RST_SW, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Panic) == ESP_RST_PANIC, "reset enum drift");
static_assert(static_cast<int>(ResetCode::IntWdt) == ESP_RST_INT_WDT, "reset enum drift");
static_assert(static_cast<int>(ResetCode::TaskWdt) == ESP_RST_TASK_WDT, "reset enum drift");
static_assert(static_cast<int>(ResetCode::OtherWdt) == ESP_RST_WDT, "reset enum drift");
static_assert(static_cast<int>(ResetCode::DeepSleep) == ESP_RST_DEEPSLEEP, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Brownout) == ESP_RST_BROWNOUT, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Sdio) == ESP_RST_SDIO, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Usb) == ESP_RST_USB, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Jtag) == ESP_RST_JTAG, "reset enum drift");
static_assert(static_cast<int>(ResetCode::Efuse) == ESP_RST_EFUSE, "reset enum drift");
static_assert(static_cast<int>(ResetCode::PwrGlitch) == ESP_RST_PWR_GLITCH, "reset enum drift");
static_assert(static_cast<int>(ResetCode::CpuLockup) == ESP_RST_CPU_LOCKUP, "reset enum drift");
static_assert(CONFIG_APP_RETRIEVE_LEN_ELF_SHA == 64, "full dump identity required");
#if defined(CONFIG_ESP_COREDUMP_CHECK_BOOT) || defined(CONFIG_ESP_COREDUMP_CAPTURE_DRAM)
#error "Use the bounded project reader; keep SDK boot parsing and DRAM capture disabled"
#endif

static_assert(offsetof(StaticTask_t, ucDummy7) == 52 &&
              sizeof(StaticTask_t::ucDummy7) == 16 && configMAX_TASK_NAME_LEN == 16,
              "review the pinned writer's actual task-name ABI");
#if defined(CONFIG_IDF_TARGET_ARCH_XTENSA)
static_assert(sizeof(StaticTask_t) == 100 && sizeof(XtExcFrame) == 112 &&
              offsetof(XtExcFrame, pc) == 4 && offsetof(XtExcFrame, a0) == 12 &&
              offsetof(XtExcFrame, a1) == 16, "review Xtensa dump ABI");
static constexpr CoredumpAbi kAbi{true, sizeof(StaticTask_t), offsetof(StaticTask_t, ucDummy7),
    sizeof(StaticTask_t::ucDummy7), sizeof(XtExcFrame), offsetof(XtExcFrame, pc),
    offsetof(XtExcFrame, a1), offsetof(XtExcFrame, a0), 152};
static constexpr uint16_t kMachine = 94;
#else
static_assert(sizeof(StaticTask_t) == 96 && sizeof(RvExcFrame) == 148 &&
              offsetof(RvExcFrame, mepc) == 0, "review RISC-V dump ABI");
static constexpr CoredumpAbi kAbi{false, sizeof(StaticTask_t), offsetof(StaticTask_t, ucDummy7),
    sizeof(StaticTask_t::ucDummy7), sizeof(RvExcFrame), offsetof(RvExcFrame, mepc), 0, 0, 8};
static constexpr uint16_t kMachine = 243;
#endif
// Preserve the same checked ABI values as compile-time parser inputs. The generic
// host entry still accepts dynamic ABI fixtures and exercises every refusal guard.
struct CompiledCoredumpAbi {
    static constexpr bool xtensa = kAbi.xtensa;
    static constexpr uint32_t tcb_bytes = kAbi.tcb_bytes, name_offset = kAbi.name_offset;
    static constexpr uint32_t name_bytes = kAbi.name_bytes, frame_bytes = kAbi.frame_bytes;
    static constexpr uint32_t pc_offset = kAbi.pc_offset, sp_offset = kAbi.sp_offset;
    static constexpr uint32_t a0_offset = kAbi.a0_offset, extra_bytes = kAbi.extra_bytes;
};
// Fixed outer/INFO/EXTRA schema is source-bound to IDF6.1's espcoredump writers.
// EXTRA contains the crashed TCB first and ISR flag last; no private SDK unwinder is called.
static constexpr uint32_t kVersion = (uint32_t{CONFIG_IDF_FIRMWARE_CHIP_ID} << 16) | 0x0104;
enum Admission : uint32_t { NoArtifact, Quarantined, Foreign, Current };
static constexpr uint32_t kStateMask = 7, kGeneration = 8;
// Capture is completed before service tasks. This payload never changes afterward;
// erase revokes atomic admission only, so borrowed views remain memory-safe.
struct BootCrashInfo : CrashFacts {
    std::array<char, 17> task{};
    uint32_t pc{0};
    std::array<uint32_t, kCrashBacktraceMax> backtrace{};
    uint8_t depth{0};
    std::array<char, 65> elf_sha256{}, dump_elf_sha256{};
};
static_assert(std::is_trivially_copyable<BootCrashInfo>::value, "complete boot publication cannot allocate");
static_assert(sizeof(BootCrashInfo) <= 256, "bounded immutable boot metadata");
static_assert(kCrashBacktraceMax == kCoredumpBacktraceMax, "one frame bound");
static BootCrashInfo s_boot;
static CrashInfoView crash_borrow(const BootCrashInfo& owner) noexcept {
    CrashInfoView view;
    static_cast<CrashFacts&>(view) = owner;
    view.task = owner.task.data();
    view.pc = owner.pc;
    view.backtrace = {owner.backtrace.data(), owner.depth};
    view.elf_sha256 = owner.elf_sha256.data();
    view.dump_elf_sha256 = owner.dump_elf_sha256.data();
    return view;
}
static CrashResetInfo s_reset{0, false};
static CoredumpHeader s_header;
static std::atomic<uint32_t> s_admission{NoArtifact};
static std::atomic<bool> s_dismissed{false};
static std::atomic<bool> s_query_error{false};
static std::atomic_flag s_dump_owner = ATOMIC_FLAG_INIT;

static Admission state(uint32_t value) { return static_cast<Admission>(value & kStateMask); }
static void invalidate(Admission next) noexcept {
    uint32_t previous = s_admission.load(std::memory_order_acquire);
    while (!s_admission.compare_exchange_weak(previous,
              ((previous + kGeneration) & ~kStateMask) | next,
              std::memory_order_acq_rel, std::memory_order_acquire)) {}
}
CoredumpLease::CoredumpLease() noexcept
    : held_(!s_dump_owner.test_and_set(std::memory_order_acquire)) {}
CoredumpLease::~CoredumpLease() { if (held_) s_dump_owner.clear(std::memory_order_release); }
const CrashResetInfo& diag_crash_reset_info() noexcept { return s_reset; }

struct PartitionQuery { const esp_partition_t* part{nullptr}; bool error{false}; };
static PartitionQuery locate_partition() {
    struct Iterator {
        esp_partition_iterator_t value{nullptr};
        ~Iterator() { if (value) esp_partition_iterator_release(value); }
    } iterator;
    // A NOT_FOUND from a filtered lookup can also mean the table loader failed
    // (e.g. missing MD5). First require a successfully loaded, nonempty registry.
    if (esp_partition_find_err(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY,
                              nullptr, &iterator.value) != ESP_OK || !iterator.value)
        return {nullptr, true};
    const esp_partition_t* found = nullptr;
    unsigned entries = 0;
    for (; iterator.value && entries < 128; ++entries) {
        const auto* part = esp_partition_get(iterator.value);
        if (!part) return {nullptr, true};
        if (part->type == ESP_PARTITION_TYPE_DATA && part->subtype == ESP_PARTITION_SUBTYPE_DATA_COREDUMP) {
            // Refuse duplicates or unexpected extents before read/SDK erase.
            if (found || part->address != 0x12000 || part->size != kCoredumpCapacity)
                return {nullptr, true};
            found = part;
        }
        iterator.value = esp_partition_next(iterator.value); // releases at end
    }
    if (iterator.value) return {nullptr, true};
    return {found, false}; // absence only after a complete, successful table walk
}
static bool same_header(const CoredumpHeader& a, const CoredumpHeader& b) {
    return a.length == b.length && a.version == b.version && a.chip_revision == b.chip_revision;
}
struct HeaderQuery { const esp_partition_t* part{nullptr}; CoredumpHeader header; bool error{false}; };
// Share the bounded reader instantiation across boot admission and live header checks.
// Its only state is the already validated partition; neither caller owns mutable input data.
struct PartitionReader {
    const esp_partition_t* part;
    bool operator()(uint32_t offset, void* data, uint32_t bytes) const {
        return esp_partition_read(part, offset, data, bytes) == ESP_OK;
    }
};
static HeaderQuery query_header() {
    const auto query = locate_partition();
    HeaderQuery out; out.part = query.part; out.error = query.error;
    if (!out.part) return out;
    PartitionReader io{out.part};
    CoredumpInput input(io, out.part->size);
    out.header = input.admit(); out.error = input.failed();
    return out;
}
CrashInfoView diag_crash_info_live() {
    const uint32_t before = s_admission.load(std::memory_order_acquire);
    HeaderQuery query;
    try { query = query_header(); } catch (...) { query.error = true; }
    if (query.error) { s_query_error.store(true, std::memory_order_release); invalidate(Quarantined); }
    else if (!query.header.present) {
        if (state(before) != NoArtifact) invalidate(NoArtifact);
    } else if ((state(before) == Current || state(before) == Foreign) &&
               !same_header(query.header, s_header)) {
        invalidate(Quarantined);
    }
    CrashInfoView info = crash_borrow(s_boot); // static immutable payload; no copy/allocation/leaf lock
    info.dismissed = s_dismissed.load(std::memory_order_acquire);
    info.artifact_present = query.header.present;
    info.query_error = query.error || s_query_error.load(std::memory_order_acquire);
    info.raw_available = query.header.readable && !query.error;
    const uint32_t after = s_admission.load(std::memory_order_acquire);
    crash_finish_snapshot(info, {before, after, same_header(query.header, s_header),
        query.error, query.header.readable, state(after) == Current, state(after) == Foreign,
        state(after) == NoArtifact});
    return info;
}

static void capture_artifact(BootCrashInfo& captured, const char* running) {
    const auto query = locate_partition();
    if (query.error) { captured.query_error = true; s_admission.store(Quarantined); return; }
    if (!query.part) { s_admission.store(NoArtifact); return; }
    PartitionReader read{query.part};
    CoredumpInput input(read, query.part->size);
    const auto outer = input.admit(); s_header = outer;
    captured.artifact_present = outer.present; captured.raw_available = outer.readable;
    captured.query_error = input.failed();
    if (!outer.present && !input.failed()) { s_admission.store(NoArtifact); return; }
    s_admission.store(Quarantined);
    if (!outer.readable || input.failed()) return;
    const bool checksum = verify_coredump_checksum(input, outer);
    const auto metadata = parse_coredump_metadata(input, outer, kMachine, kVersion,
                                                 running, std::strlen(running), checksum);
    CoredumpSummary summary;
    if (metadata.identity == DumpIdentity::VerifiedCurrent) {
        summary = parse_coredump_summary_for_arch<kAbi.xtensa>(input, metadata, CompiledCoredumpAbi{}, [](uint32_t pc) {
            return esp_ptr_executable(reinterpret_cast<const void*>(static_cast<uintptr_t>(pc)));
        });
    }
    captured.query_error = input.failed();
    if (input.failed()) return; // never admit a previously verified image after a later read failure
    captured.checksum_verified = checksum;
    captured.dump_identity = metadata.identity;
    captured.foreign_dump = metadata.identity == DumpIdentity::Foreign;
    std::memcpy(captured.dump_elf_sha256.data(), metadata.hash, sizeof(metadata.hash));
    if (summary.available) {
        if (summary.depth > captured.backtrace.size()) { captured.query_error = true; return; }
        std::memcpy(captured.task.data(), summary.task, sizeof(summary.task));
        captured.pc = summary.pc;
        captured.depth = summary.depth;
        std::memcpy(captured.backtrace.data(), summary.backtrace, summary.depth * sizeof(uint32_t));
        captured.summary_available = true;
        captured.corrupted = summary.corrupted; captured.truncated = summary.truncated;
        captured.isr = summary.isr;
    }
    captured.coredump = metadata.identity == DumpIdentity::VerifiedCurrent;
    s_admission.store(captured.coredump ? Current : captured.foreign_dump ? Foreign : Quarantined,
                      std::memory_order_release);
}
// Reuse the bounded CRASH records already emitted by Syslog. No second human
// formatter is linked into firmware; complete running identity survives no-summary cases.
static void log_boot_crash(const CrashInfoView& info) noexcept {
    try {
        if (!crash_is_notable(info)) return;
        std::vector<std::string> lines;
        build_crash_log_lines(info, lines);
        for (const auto& line : lines) ESP_LOGE(TAG, "%s", line.c_str());
        if (!info.summary_available && !info.elf_sha256.empty())
            ESP_LOGE(TAG, "elf_sha256=%.64s", info.elf_sha256.data());
    } catch (...) {} // formatting/partial logging never changes admission or retries
}
void diag_crash_capture() noexcept {
    s_dismissed.store(false, std::memory_order_release);
    s_reset = {static_cast<int>(esp_reset_reason()), false};
    s_reset.fault = reset_is_fault(s_reset.code);
    crash_set_reset(s_boot, s_reset.code);
    try {
        BootCrashInfo captured; crash_set_reset(captured, s_reset.code);
        esp_app_get_elf_sha256(captured.elf_sha256.data(), captured.elf_sha256.size());
        capture_artifact(captured, captured.elf_sha256.data());
        s_boot = captured; // publish only a complete fixed boot-owned object
        s_query_error.store(s_boot.query_error, std::memory_order_release);
    } catch (...) {
        s_boot.query_error = true;
        s_query_error.store(true, std::memory_order_release);
        invalidate(Quarantined);
    }
    log_boot_crash(crash_borrow(s_boot));
}

CoredumpOpenResult diag_crash_open_dump(const CoredumpLease& lease, bool raw, CoredumpDownload& out) {
    out = {};
    if (!lease.held()) return CoredumpOpenResult::Busy;
    const uint32_t before = s_admission.load(std::memory_order_acquire);
    HeaderQuery query;
    try { query = query_header(); } catch (...) { query.error = true; }
    if (query.error) {
        s_query_error.store(true, std::memory_order_release); invalidate(Quarantined);
        return CoredumpOpenResult::Error;
    }
    if (!query.header.present) { invalidate(NoArtifact); return CoredumpOpenResult::Absent; }
    if (!query.header.readable) { invalidate(Quarantined); return CoredumpOpenResult::Unverified; }
    if ((state(before) == Current || state(before) == Foreign) &&
        !same_header(query.header, s_header)) invalidate(Quarantined);
    if (!raw && (!same_header(query.header, s_header) ||
        before != s_admission.load(std::memory_order_acquire) || state(before) != Current))
        return CoredumpOpenResult::Unverified;
    out = {query.part, query.header.length};
    return CoredumpOpenResult::Ready;
}
CrashEraseResult diag_crash_erase(bool dismiss, EffectReplyState& reply) noexcept {
    CoredumpLease lease;
    if (!lease.held()) return CrashEraseResult::Busy;
    try {
        const auto query = locate_partition();
        if (!query.error && !query.part) {
            invalidate(NoArtifact); s_query_error.store(false, std::memory_order_release);
            if (dismiss) s_dismissed.store(true, std::memory_order_release);
            reply.succeeded(); return CrashEraseResult::Absent;
        }
        // Conservative before any possible erase/exception; never restore Current during this boot.
        reply.attempting(RequestEffect::CoredumpErase); invalidate(Quarantined);
        if (query.error) { s_query_error.store(true, std::memory_order_release); return CrashEraseResult::Unconfirmed; }
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
        if (esp_core_dump_image_erase() != ESP_OK) {
            s_query_error.store(true, std::memory_order_release); return CrashEraseResult::Unconfirmed;
        }
        invalidate(NoArtifact); s_query_error.store(false, std::memory_order_release);
        if (dismiss) s_dismissed.store(true, std::memory_order_release);
        reply.succeeded(); return CrashEraseResult::Erased;
#else
        return CrashEraseResult::Unconfirmed;
#endif
    } catch (...) {
        reply.attempting(RequestEffect::CoredumpErase); invalidate(Quarantined);
        s_query_error.store(true, std::memory_order_release);
        return CrashEraseResult::Unconfirmed;
    }
}
}  // namespace tk
