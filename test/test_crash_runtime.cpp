// Compile the actual diagnostic shell, including its atomics/lease/exception owner.
// Only SDK/partition/PSA I/O is passive. Cold-boot fixture reset is test-only.
#include "diag_crash.cpp"
#include "coredump_fixture.hpp"
#include <algorithm>
#include <cstdio>
#include <cinttypes>
#include <cstdlib>
#include <new>

namespace {
unsigned checks{};
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr, "FAIL crash runtime:%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
long allocation_at = -1;
bool allocation_hit{};
unsigned allocations{};
enum class HashPhase { None, Init, Setup, Update, Verify, Abort };
struct Fixture {
    esp_partition_t partition{0x12000, 0xc000};
    esp_err_t lookup{ESP_OK}, erase{ESP_OK};
    esp_reset_reason_t reset{ESP_RST_PANIC};
    bool throw_lookup{}, throw_erase{}, throw_log{}, erase_then_fail{}, race_erase{};
    bool no_coredump{}, duplicate_coredump{}, endless_registry{}, null_iterator{}, null_get{};
    unsigned registry_entries{2};
    unsigned iterator_owners{}, iterator_releases{};
    unsigned reads{}, fail_read{}, throw_read{}, erases{}, logs{}, hash_abort{};
    char log_lines[16][256]{};
    unsigned fail_log_at{UINT32_MAX};
    size_t hashed{};
    HashPhase fail_hash{HashPhase::None}, throw_hash{HashPhase::None};
    std::string running = std::string(64, 'a');
    std::vector<uint8_t> image;
} fake;
constexpr bool xtensa =
#if defined(HOST_CRASH_RISCV)
    false;
#else
    true;
#endif
constexpr uint32_t version = (uint32_t{CONFIG_IDF_FIRMWARE_CHIP_ID} << 16) | 0x0104;
void cold_boot() {
    allocations = 0;
    allocation_at = -1; allocation_hit = false;
    tk::s_boot = {}; tk::s_reset = {}; tk::s_header = {};
    tk::s_admission.store(tk::NoArtifact); tk::s_dismissed.store(false);
    tk::s_query_error.store(false); tk::s_dump_owner.clear();
    fake = Fixture{};
    fake.image = dump_test_summary_image(xtensa, version, 2);
}
bool hash_error(HashPhase phase) {
    if (phase == fake.throw_hash) throw std::bad_alloc();
    return phase == fake.fail_hash;
}
tk::CoredumpOpenResult open(bool raw = false) {
    tk::CoredumpLease lease;
    tk::CoredumpDownload result;
    const auto code = tk::diag_crash_open_dump(lease, raw, result);
    if (code == tk::CoredumpOpenResult::Ready)
        CHECK(result.partition == &fake.partition && result.bytes == fake.image.size());
    else CHECK(result.partition == nullptr && result.bytes == 0);
    return code;
}
}  // namespace

void* operator new(size_t bytes) {
    ++allocations;
    if (allocation_at == 0) { allocation_at = -1; allocation_hit = true; throw std::bad_alloc(); }
    if (allocation_at > 0) --allocation_at;
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

esp_err_t esp_partition_find_err(int type, int subtype, const char* label,
                               esp_partition_iterator_t* result) {
    CHECK(type == ESP_PARTITION_TYPE_ANY && subtype == ESP_PARTITION_SUBTYPE_ANY && !label);
    if (fake.throw_lookup) throw std::bad_alloc();
    *result = nullptr;
    if (fake.lookup != ESP_OK || fake.null_iterator) return fake.lookup;
    static host_partition_iterator iterator;
    CHECK(fake.iterator_owners == 0); iterator.index = 0;
    *result = &iterator; ++fake.iterator_owners;
    return ESP_OK;
}
const esp_partition_t* esp_partition_get(esp_partition_iterator_t it) {
    CHECK(it && fake.iterator_owners == 1);
    static const esp_partition_t anchor{0x20000, 0x1f0000, 0, 16};
    if (fake.null_get) return nullptr;
    const bool core = it->index == 1 || (fake.duplicate_coredump && it->index == 2);
    return core && !fake.no_coredump && !fake.endless_registry ? &fake.partition : &anchor;
}
void esp_partition_iterator_release(esp_partition_iterator_t it) {
    CHECK(it && fake.iterator_owners == 1); --fake.iterator_owners; ++fake.iterator_releases;
}
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it) {
    CHECK(it && fake.iterator_owners == 1);
    if (++it->index < fake.registry_entries || fake.endless_registry)
        return it;
    esp_partition_iterator_release(it); return nullptr;
}
esp_err_t esp_partition_read(const esp_partition_t* part, size_t offset, void* data, size_t bytes) {
    ++fake.reads;
    CHECK(part == &fake.partition && part->address == 0x12000 && part->size == 0xc000);
    CHECK(bytes <= 256 && offset <= part->size && bytes <= part->size - offset);
    if (fake.reads == fake.throw_read) throw std::bad_alloc();
    if (fake.reads == fake.fail_read) return ESP_FAIL;
    std::memset(data, 0xff, bytes);
    if (offset < fake.image.size())
        std::memcpy(data, fake.image.data() + offset, std::min(bytes, fake.image.size() - offset));
    if (fake.race_erase) {
        fake.race_erase = false;
        tk::EffectReplyState reply;
        CHECK(tk::diag_crash_erase(false, reply) == tk::CrashEraseResult::Erased);
    }
    return ESP_OK;
}
esp_err_t esp_core_dump_image_erase() {
    ++fake.erases;
    // Admission must be revoked BEFORE the SDK can mutate or throw.
    CHECK(tk::state(tk::s_admission.load()) != tk::Current);
    if (fake.throw_erase) throw std::bad_alloc();
    if (fake.erase == ESP_OK || fake.erase_then_fail) fake.image.clear();
    return fake.erase;
}
esp_reset_reason_t esp_reset_reason() { return fake.reset; }
int esp_app_get_elf_sha256(char* output, size_t length) {
    CHECK(length == 65);
    const auto bytes = std::min(length - 1, fake.running.size());
    std::memcpy(output, fake.running.data(), bytes); output[bytes] = 0;
    return static_cast<int>(bytes);
}
bool esp_ptr_executable(const void* pointer) {
    const auto pc = reinterpret_cast<uintptr_t>(pointer);
    return pc >= 0x40000000 && pc < 0x44000000;
}
void host_crash_log(char level, const char* tag, const char* format, const char* value) {
    CHECK(level == 'E');
    CHECK(std::strcmp(tag, "diag_crash") == 0);
    const auto index = fake.logs++;
    if (fake.throw_log || index == fake.fail_log_at) throw std::bad_alloc();
    CHECK(index < 16);
    const int bytes = std::snprintf(fake.log_lines[index], sizeof(fake.log_lines[index]), format, value);
    CHECK(bytes >= 0 && static_cast<size_t>(bytes) < sizeof(fake.log_lines[index]));
    CHECK(static_cast<size_t>(bytes) <= tk::kBootlogLineMax);
    // Pinned IDF6.1 LOG_FORMAT, configured with no colors and uint32 RTOS timestamps.
    char prefixed[256];
    const int total = std::snprintf(prefixed, sizeof(prefixed), "E (%" PRIu32 ") %s: %s\n",
                                    UINT32_MAX, tag, fake.log_lines[index]);
    CHECK(total >= 0 && static_cast<size_t>(total) < sizeof(prefixed));
}
extern "C" psa_status_t psa_crypto_init() { return hash_error(HashPhase::Init) ? -1 : PSA_SUCCESS; }
extern "C" psa_status_t psa_hash_setup(psa_hash_operation_t* op, psa_algorithm_t algorithm) {
    CHECK(algorithm == PSA_ALG_SHA_256); op->active = 1; fake.hashed = 0;
    return hash_error(HashPhase::Setup) ? -1 : PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_update(psa_hash_operation_t* op, const uint8_t* data, size_t bytes) {
    CHECK(op->active && bytes <= 256 && fake.hashed + bytes <= fake.image.size() - 32);
    CHECK(std::memcmp(fake.image.data() + fake.hashed, data, bytes) == 0);
    fake.hashed += bytes;
    return hash_error(HashPhase::Update) ? -1 : PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_verify(psa_hash_operation_t* op, const uint8_t* data, size_t bytes) {
    CHECK(op->active && bytes == 32 && fake.hashed == fake.image.size() - 32);
    CHECK(std::memcmp(fake.image.data() + fake.hashed, data, bytes) == 0);
    return hash_error(HashPhase::Verify) ? -1 : PSA_SUCCESS;
}
extern "C" psa_status_t psa_hash_abort(psa_hash_operation_t* op) {
    ++fake.hash_abort; op->active = 0;
    return hash_error(HashPhase::Abort) ? -1 : PSA_SUCCESS;
}

int main() {
    cold_boot(); tk::diag_crash_capture();
    auto info = tk::diag_crash_info_live();
    CHECK(info.fault && info.coredump && info.artifact_present && info.raw_available);
    CHECK(info.summary_available && info.task == "vehicle_loop" && info.checksum_verified);
    CHECK(info.dump_identity == tk::DumpIdentity::VerifiedCurrent && !info.query_error);
    CHECK(tk::diag_crash_reset_info().code == ESP_RST_PANIC && tk::diag_crash_reset_info().fault);
    CHECK(open() == tk::CoredumpOpenResult::Ready && open(true) == tk::CoredumpOpenResult::Ready);
    CHECK(fake.erases == 0 && fake.hash_abort == 1);
    CHECK(fake.logs == (xtensa ? 3U : 2U));
    bool full_running_identity = false;
    for (unsigned i = 0; i < fake.logs; ++i) {
        const std::string_view line(fake.log_lines[i]);
        CHECK(line.size() <= tk::kBootlogLineMax && line.find('\n') == std::string_view::npos);
        full_running_identity |= line.find(fake.running) != std::string_view::npos;
    }
    CHECK(full_running_identity);

    const unsigned capture_reads = fake.reads - 3; // three live/open header probes above

    // Exercise the fixed boot-owned copy at both bounds through the actual capture shell.
    cold_boot(); fake.image = dump_test_summary_image(xtensa, version, 16);
    const auto full_tcb = 12 + tk::dump_u32(fake.image.data() + 12 + 52 + 32 + 4);
    std::memcpy(fake.image.data() + full_tcb + 52, "abcdefghijklmnop", 16);
    tk::diag_crash_capture(); info = tk::diag_crash_info_live();
    CHECK(info.coredump && info.summary_available && !info.query_error);
    CHECK(info.task == "abcdefghijklmnop" && tk::s_boot.task[16] == 0);
    CHECK(info.elf_sha256 == fake.running && info.dump_elf_sha256 == fake.running);
    CHECK(tk::s_boot.elf_sha256[64] == 0 && tk::s_boot.dump_elf_sha256[64] == 0);
    CHECK(tk::s_boot.depth == (xtensa ? 16 : 0));
    CHECK(info.backtrace.size() == (xtensa ? 16 : 0));
    if (xtensa) {
        CHECK(!info.corrupted && !info.truncated);
        CHECK(info.backtrace[0] == 0x40001000 && info.backtrace[15] == 0x400020e0);
        CHECK(tk::s_boot.backtrace[15] == 0x400020e0);
    } else {
        CHECK(info.pc == 0x42001000 && !info.corrupted);
    }


    // A damaged zero PC and unnamed task remain an explicit summary with its flags.
    cold_boot();
    const auto tcb = 12 + tk::dump_u32(fake.image.data() + 12 + 52 + 32 + 4);
    const auto stack = 12 + tk::dump_u32(fake.image.data() + 12 + 52 + 64 + 4);
    const auto extra = 12 + tk::dump_u32(fake.image.data() + 12 + 52 + 4) + 104 + 12 + 16;
    std::fill(fake.image.begin() + tcb + 52, fake.image.begin() + tcb + 68, 0);
    dump_test_u32(fake.image, stack + (xtensa ? 4 : 0), 0);
    dump_test_u32(fake.image, extra + (xtensa ? 148 : 4), 1);
    tk::diag_crash_capture(); info = tk::diag_crash_info_live();
    CHECK(info.summary_available && info.pc == 0 && info.task.empty() && info.corrupted && info.isr);
    CHECK(info.coredump && !info.query_error && fake.erases == 0);

    // An unrelated build's valid dump survives every boot; default download is quarantined.
    cold_boot(); fake.running.assign(64, 'b'); fake.reset = ESP_RST_SW; tk::diag_crash_capture();
    info = tk::diag_crash_info_live();
    CHECK(info.foreign_dump && info.dump_identity == tk::DumpIdentity::Foreign);
    CHECK(info.artifact_present && info.raw_available && !info.coredump && !info.summary_available);
    CHECK(info.checksum_verified && fake.erases == 0 && tk::crash_is_notable(info));
    CHECK(fake.logs == 2); // foreign header plus a complete running-ELF fallback
    CHECK(std::string_view(fake.log_lines[0]).find(std::string(64, 'a')) != std::string_view::npos);
    CHECK(std::string_view(fake.log_lines[1]) == "elf_sha256=" + fake.running);

    CHECK(open() == tk::CoredumpOpenResult::Unverified && open(true) == tk::CoredumpOpenResult::Ready);
    cold_boot(); fake.running.resize(8); tk::diag_crash_capture();
    CHECK(!tk::diag_crash_info_live().coredump && open(true) == tk::CoredumpOpenResult::Ready);

    for (HashPhase phase : {HashPhase::Init, HashPhase::Setup, HashPhase::Update,
                           HashPhase::Verify, HashPhase::Abort}) {
        cold_boot(); fake.fail_hash = phase; tk::diag_crash_capture();
        info = tk::diag_crash_info_live();
        CHECK(!info.coredump && !info.summary_available && !info.checksum_verified && fake.erases == 0);
        CHECK(open() == tk::CoredumpOpenResult::Unverified && open(true) == tk::CoredumpOpenResult::Ready);
        cold_boot(); fake.throw_hash = phase; tk::diag_crash_capture();
        CHECK(tk::diag_crash_info_live().query_error && !tk::diag_crash_info_live().coredump);
    }
    // Every actual boot read failure/exception is contained; no automatic write is available.
    for (unsigned read = 1; read <= capture_reads; ++read) {
        cold_boot(); fake.fail_read = read; tk::diag_crash_capture();
        CHECK(fake.reads == read && fake.erases == 0 && tk::diag_crash_info_live().query_error);
        CHECK(open() != tk::CoredumpOpenResult::Ready);
        cold_boot(); fake.throw_read = read; tk::diag_crash_capture();
        CHECK(fake.erases == 0 && tk::diag_crash_info_live().query_error);
    }
    for (esp_err_t error : {ESP_ERR_NO_MEM, ESP_FAIL, ESP_ERR_NOT_FOUND}) {
        cold_boot(); fake.lookup = error; tk::diag_crash_capture();
        CHECK(tk::diag_crash_info_live().query_error && fake.reads == 0 && fake.erases == 0);
        tk::EffectReplyState reply;
        CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Unconfirmed);
        CHECK(!tk::s_dismissed.load() && reply.outcome == tk::RequestOutcome::Uncertain && fake.erases == 0);
    }
    cold_boot(); fake.throw_lookup = true; tk::diag_crash_capture();
    CHECK(tk::diag_crash_info_live().query_error && fake.reads == 0 && fake.erases == 0);
    tk::EffectReplyState lookup_reply;
    CHECK(tk::diag_crash_erase(true, lookup_reply) == tk::CrashEraseResult::Unconfirmed);
    CHECK(!tk::s_dismissed.load() && !lookup_reply.success() && fake.erases == 0);
    for (uint32_t length : {0U, 1U, 95U, tk::kCoredumpCapacity + 32}) {
        cold_boot(); dump_test_u32(fake.image, 0, length); tk::diag_crash_capture();
        info = tk::diag_crash_info_live();
        CHECK(info.artifact_present && !info.raw_available && !info.coredump && fake.erases == 0);
        CHECK(open(true) == tk::CoredumpOpenResult::Unverified);
    }
    cold_boot(); fake.image.clear(); tk::diag_crash_capture();
    CHECK(!tk::diag_crash_info_live().artifact_present && open() == tk::CoredumpOpenResult::Absent);
    for (unsigned mismatch = 0; mismatch < 2; ++mismatch) {
        cold_boot(); if (mismatch) --fake.partition.size; else --fake.partition.address;
        tk::diag_crash_capture();
        CHECK(tk::diag_crash_info_live().query_error && fake.reads == 0);
        tk::EffectReplyState reply;
        CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Unconfirmed && fake.erases == 0);
    }
    cold_boot(); fake.no_coredump = true; tk::diag_crash_capture();
    tk::EffectReplyState reply;
    CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Absent);
    CHECK(reply.success() && tk::s_dismissed.load() && fake.erases == 0);

    for (bool endless : {false, true}) {
        cold_boot(); fake.endless_registry = endless; fake.duplicate_coredump = !endless; fake.registry_entries = 3;
        tk::diag_crash_capture(); reply = {};
        CHECK(tk::diag_crash_info_live().query_error && fake.iterator_owners == 0);
        CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Unconfirmed);
        CHECK(!reply.success() && !tk::s_dismissed.load() && fake.erases == 0 && fake.iterator_owners == 0);
    }

    for (bool missing : {false, true}) {
        cold_boot(); fake.registry_entries = 128; fake.no_coredump = missing;
        tk::diag_crash_capture(); info = tk::diag_crash_info_live();
        CHECK(!info.query_error && info.coredump == !missing && fake.iterator_owners == 0);
        cold_boot(); fake.registry_entries = 129; fake.no_coredump = missing;
        tk::diag_crash_capture(); info = tk::diag_crash_info_live();
        CHECK(info.query_error && !info.coredump && fake.iterator_owners == 0);
        reply = {}; CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Unconfirmed);
        CHECK(fake.erases == 0 && !reply.success() && !tk::s_dismissed.load());
    }
    for (bool null_iterator : {false, true}) {
        cold_boot(); fake.null_iterator = null_iterator; fake.null_get = !null_iterator;
        tk::diag_crash_capture(); reply = {};
        CHECK(tk::diag_crash_info_live().query_error && fake.iterator_owners == 0);
        CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Unconfirmed);
        CHECK(fake.erases == 0 && !reply.success() && !tk::s_dismissed.load());
    }

    // An SDK erase error, including NOT_FOUND after entry, cannot become confirmed absence.
    for (esp_err_t error : {ESP_FAIL, ESP_ERR_NOT_FOUND}) {
        cold_boot(); tk::diag_crash_capture(); fake.erase = error; reply = {};
        CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Unconfirmed);
        CHECK(fake.erases == 1 && !tk::s_dismissed.load() && !reply.success());
        CHECK(!tk::diag_crash_info_live().summary_available && open() == tk::CoredumpOpenResult::Unverified);
        CHECK(open(true) == tk::CoredumpOpenResult::Ready);
    }
    cold_boot(); tk::diag_crash_capture(); fake.throw_erase = true; reply = {};
    CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Unconfirmed);
    CHECK(!reply.success() && fake.erases == 1 && !tk::s_dismissed.load());
    cold_boot(); tk::diag_crash_capture(); fake.erase_then_fail = true; fake.erase = ESP_FAIL; reply = {};
    CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Unconfirmed);
    CHECK(!reply.success() && !tk::s_dismissed.load());
    CHECK(open() == tk::CoredumpOpenResult::Absent && tk::diag_crash_info_live().query_error);
    for (bool dismiss : {false, true}) {
        cold_boot(); tk::diag_crash_capture(); reply = {};
        CHECK(tk::diag_crash_erase(dismiss, reply) == tk::CrashEraseResult::Erased);
        CHECK(reply.success() && tk::s_dismissed.load() == dismiss && fake.erases == 1);
        CHECK(!tk::diag_crash_info_live().coredump && open() == tk::CoredumpOpenResult::Absent);
    }
    cold_boot(); tk::diag_crash_capture();
    { tk::CoredumpLease owner; CHECK(owner.held()); reply = {};
      CHECK(open() == tk::CoredumpOpenResult::Busy);
      CHECK(tk::diag_crash_erase(true, reply) == tk::CrashEraseResult::Busy);
      CHECK(!reply.needs_effect_reply() && fake.erases == 0 && !tk::s_dismissed.load()); }
    CHECK(open() == tk::CoredumpOpenResult::Ready);
    fake.race_erase = true; info = tk::diag_crash_info_live();
    CHECK(!info.coredump && !info.summary_available && !info.raw_available && fake.erases == 1);

    // The live payload no longer performs the old string/vector copy allocation.
    // The passive SDK lookup here does not allocate; the real SDK still may.
    cold_boot(); tk::diag_crash_capture();
    const unsigned previous_allocations = allocations;
    allocation_at = 0;
    info = tk::diag_crash_info_live();
    CHECK(allocations == previous_allocations && !allocation_hit);
    allocation_at = -1;
    CHECK(info.task.data() == tk::s_boot.task.data());
    CHECK(info.backtrace.size() == tk::s_boot.depth);
    if (tk::s_boot.depth) CHECK(info.backtrace.begin() == tk::s_boot.backtrace.data());
    else CHECK(info.backtrace.begin() == info.backtrace.end());
    CHECK(info.elf_sha256.data() == tk::s_boot.elf_sha256.data());
    const auto retained = info;
    tk::EffectReplyState erased;
    CHECK(tk::diag_crash_erase(false, erased) == tk::CrashEraseResult::Erased);
    CHECK(retained.task == "vehicle_loop" && retained.summary_available);
    info = tk::diag_crash_info_live();
    CHECK(!info.coredump && !info.summary_available && !info.raw_available);
    CHECK(!info.artifact_present && std::string_view(tk::s_boot.task.data()) == "vehicle_loop");
    CHECK(open() == tk::CoredumpOpenResult::Absent); // stale metadata is never export authority

    cold_boot(); tk::diag_crash_capture(); fake.fail_read = fake.reads + 1;
    CHECK(tk::diag_crash_info_live().query_error);
    fake.fail_read = 0;
    CHECK(!tk::diag_crash_info_live().coredump && open() == tk::CoredumpOpenResult::Unverified);
    CHECK(open(true) == tk::CoredumpOpenResult::Ready);
    cold_boot(); tk::diag_crash_capture(); dump_test_u32(fake.image, 8, 7);
    CHECK(open() == tk::CoredumpOpenResult::Unverified);
    dump_test_u32(fake.image, 8, 0);
    CHECK(open() == tk::CoredumpOpenResult::Unverified); // changed header revokes boot admission

    // Ordinary and dismissed metadata emit no hash-only record.
    cold_boot(); fake.reset = ESP_RST_POWERON; fake.no_coredump = true;
    tk::diag_crash_capture(); CHECK(fake.logs == 0);
    auto dismissed = tk::crash_borrow(tk::s_boot); dismissed.dismissed = true;
    dismissed.query_error = true; tk::log_boot_crash(dismissed); CHECK(fake.logs == 0);
    // A sink failure after a partial report neither revokes valid evidence nor retries.
    cold_boot(); tk::diag_crash_capture();
    fake.logs = 0; fake.fail_log_at = 1;
    tk::log_boot_crash(tk::crash_borrow(tk::s_boot));
    CHECK(fake.logs == 2 && tk::state(tk::s_admission.load()) == tk::Current);
    CHECK(tk::diag_crash_info_live().summary_available && fake.erases == 0);

    // Maximum bounded fields retain the complete line under the configured SDK prefix.
    fake.logs = 0; fake.fail_log_at = UINT32_MAX;
    tk::CrashInfo maximum;
    tk::crash_set_reset(maximum, static_cast<int>(tk::ResetCode::CpuLockup));
    maximum.coredump = maximum.artifact_present = maximum.raw_available = true;
    maximum.summary_available = maximum.corrupted = maximum.truncated = maximum.isr = true;
    maximum.query_error = true; maximum.dump_identity = tk::DumpIdentity::Foreign;
    maximum.task.assign(tk::kBootlogTaskMax, 't');
    maximum.elf_sha256.assign(64, 'a'); maximum.dump_elf_sha256.assign(64, 'b');
    maximum.pc = UINT32_MAX; maximum.backtrace.assign(16, UINT32_MAX);
    tk::log_boot_crash(tk::crash_borrow(maximum));
    CHECK(fake.logs == 3);
    CHECK(std::string_view(fake.log_lines[0]).find(maximum.dump_elf_sha256) != std::string_view::npos);
    CHECK(std::string_view(fake.log_lines[1]).find(maximum.elf_sha256) != std::string_view::npos);
    CHECK(std::count(fake.log_lines[2], fake.log_lines[2] + std::strlen(fake.log_lines[2]), 'x') == 16);

    unsigned failures = 0;
    for (long at = 0; at < 40; ++at) {
        cold_boot(); allocation_at = at; tk::diag_crash_capture(); allocation_at = -1;
        CHECK(tk::diag_crash_reset_info().fault && fake.erases == 0);
        if (allocation_hit) ++failures;
        if (tk::s_query_error.load()) CHECK(!tk::diag_crash_info_live().coredump);
        else CHECK(tk::diag_crash_info_live().summary_available);
    }
    CHECK(failures >= 3);
    cold_boot(); fake.throw_log = true; tk::diag_crash_capture();
    CHECK(tk::diag_crash_info_live().coredump);
    alignas(tk::CoredumpReadBuffer) unsigned char memory[sizeof(tk::CoredumpReadBuffer)];
    auto* buffer = new (memory) tk::CoredumpReadBuffer;
    std::memset(buffer->bytes, 0xa5, sizeof(buffer->bytes)); buffer->~CoredumpReadBuffer();
    CHECK(std::all_of(std::begin(memory), std::end(memory), [](auto b) { return b == 0; }));
    std::printf("OK %u actual crash shell checks (%s facade; no hardware or SDK ABI proof)\n",
                checks, xtensa ? "Xtensa" : "RISC-V");
}
