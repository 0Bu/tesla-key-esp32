#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Pure, hardware-free logic shared by firmware and the host mock build. No IDF,
// FreeRTOS, NVS or HTTP dependency. diag_crash.cpp owns flash, hashing and publication;
// this reader interprets only serialized bytes and never dereferences a dumped address.
namespace tk {

inline constexpr uint32_t kCoredumpCapacity = 0xc000;
inline constexpr uint32_t kCoredumpHeaderBytes = 12;
inline constexpr uint32_t kCoredumpChecksumBytes = 32;
inline constexpr uint32_t kCoredumpMaxHeaders = 256;
inline constexpr uint32_t kCoredumpMaxNotes = 256;
inline constexpr uint32_t kCoredumpMaxReads = 2048;

enum class CoredumpRequestMode { Current, Raw, Clear, Invalid };
inline CoredumpRequestMode coredump_request_mode(std::string_view query) {
    if (query.empty()) return CoredumpRequestMode::Current;
    if (query == "raw=1") return CoredumpRequestMode::Raw;
    if (query == "clear=1") return CoredumpRequestMode::Clear;
    return CoredumpRequestMode::Invalid;
}

enum class DumpIdentity : uint8_t { Unverified, VerifiedCurrent, Foreign };

inline const char* dump_identity_name(DumpIdentity identity) {
    switch (identity) {
        case DumpIdentity::VerifiedCurrent: return "current";
        case DumpIdentity::Foreign: return "foreign";
        default: return "unverified";
    }
}

inline unsigned dump_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 16;
}

inline DumpIdentity classify_dump_identity(const char* running, size_t running_len,
                                          const char* dump, size_t dump_len,
                                          bool checksum_ok) {
    if (!checksum_ok || !running || !dump || running_len < 8 || dump_len < 8 ||
        running_len > 64 || dump_len > 64) return DumpIdentity::Unverified;
    for (size_t i = 0; i < running_len; ++i)
        if (dump_hex(static_cast<unsigned char>(running[i])) > 15) return DumpIdentity::Unverified;
    for (size_t i = 0; i < dump_len; ++i)
        if (dump_hex(static_cast<unsigned char>(dump[i])) > 15) return DumpIdentity::Unverified;
    const size_t overlap = running_len < dump_len ? running_len : dump_len;
    for (size_t i = 0; i < overlap; ++i)
        if (dump_hex(static_cast<unsigned char>(running[i])) !=
            dump_hex(static_cast<unsigned char>(dump[i]))) return DumpIdentity::Foreign;
    return running_len == 64 && dump_len == 64 ? DumpIdentity::VerifiedCurrent
                                             : DumpIdentity::Unverified;
}

inline uint32_t dump_u32(const uint8_t* p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t value;
    std::memcpy(&value, p, sizeof(value)); // safe for unaligned serialized input
    return value;
#else
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) |
           (uint32_t{p[3]} << 24);
#endif
}

inline uint16_t dump_u16(const uint8_t* p) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint16_t value;
    std::memcpy(&value, p, sizeof(value));
    return value;
#else
    return static_cast<uint16_t>(uint16_t{p[0]} | (uint16_t{p[1]} << 8));
#endif
}

inline bool dump_range(uint32_t offset, uint32_t length, uint32_t limit) {
    return offset <= limit && length <= limit - offset;
}

inline bool dump_align4(uint32_t value, uint32_t& aligned) {
    if (value > UINT32_MAX - 3) return false;
    aligned = (value + 3) & ~uint32_t{3};
    return true;
}

struct CoredumpHeader {
    bool present{false};
    bool readable{false};
    uint32_t length{0};
    uint32_t version{0};
    uint32_t chip_revision{0};
};

// The same budget covers initial admission, checksum reads, ELF parsing and summary
// lookups. The integration callback must return false on flash failure. A failed read
// latches failure and prevents any subsequent callback, including accidental retries.
template <class Read> class CoredumpInput {
public:
    CoredumpInput(Read& io, uint32_t partition_size) : io_(io) {
        limit_ = partition_size < kCoredumpCapacity ? partition_size : kCoredumpCapacity;
        byte_limit_ = 2 * limit_ + 4096;
    }

    bool read(uint32_t offset, void* destination, uint32_t length) {
        if (failed_ || !destination || length > 256 || !dump_range(offset, length, limit_) ||
            reads_ >= kCoredumpMaxReads || bytes_ > byte_limit_ || length > byte_limit_ - bytes_) {
            failed_ = true;
            return false;
        }
        ++reads_;
        bytes_ += length;
        if (!io_(offset, destination, length)) {
            failed_ = true;
            return false;
        }
        return true;
    }

    CoredumpHeader admit() {
        uint8_t bytes[12];
        CoredumpHeader h;
        if (admission_called_) { failed_ = true; return h; }
        admission_called_ = true;
        if (!read(0, bytes, sizeof(bytes))) return h;
        h.length = dump_u32(bytes);
        h.present = h.length != UINT32_MAX;
        h.version = dump_u32(bytes + 4);
        h.chip_revision = dump_u32(bytes + 8);
        if (!h.present || h.length < 12 + 52 + 32 || h.length % 32 != 0 ||
            h.length > limit_) return h;
        limit_ = h.length;
        byte_limit_ = 2 * limit_ + 4096;
        h.readable = true;
        image_ready_ = true;
        version_ = h.version;
        return h;
    }

    bool payload(uint32_t elf_offset, void* destination, uint32_t length) {
        // ELF-relative ranges exclude both the outer header and the checksum.
        if (!image_ready_ || limit_ < 12 + 32 || !dump_range(elf_offset, length, limit_ - 12 - 32)) {
            failed_ = true;
            return false;
        }
        return read(12 + elf_offset, destination, length);
    }

    bool failed() const { return failed_; }
    uint32_t reads() const { return reads_; }
    uint32_t bytes_read() const { return bytes_; }
    uint32_t length() const { return limit_; }
    uint32_t version() const { return version_; }
    bool image_ready() const { return image_ready_; }

private:
    Read& io_;
    uint32_t limit_{0}, byte_limit_{0}, reads_{0}, bytes_{0}, version_{0};
    bool failed_{false}, admission_called_{false}, image_ready_{false};
};

struct DumpProgramHeader {
    uint32_t type{0}, offset{0}, address{0}, bytes{0}, memory_bytes{0};
};

template <class Input>
bool dump_program_header(Input& input, uint16_t index, DumpProgramHeader& result) {
    uint8_t bytes[32];
    if (!input.payload(52 + uint32_t{index} * 32, bytes, sizeof(bytes))) return false;
    result = {dump_u32(bytes), dump_u32(bytes + 4), dump_u32(bytes + 8),
              dump_u32(bytes + 16), dump_u32(bytes + 20)};
    return true;
}

struct CoredumpMetadata {
    bool valid_elf{false};
    DumpIdentity identity{DumpIdentity::Unverified};
    uint16_t headers{0};
    uint32_t extra_offset{0}, extra_bytes{0};
    char hash[65]{};
};

// Accept the pinned writer's sequential ELF32 layout, not arbitrary executable ELF.
// Header/note/data ranges consume linear work; no attacker-controlled quadratic search.
// The caller supplies the independently checked checksum verdict. Failed integrity cannot
// grant identity, task/frame interpretation or a default current-build download.
template <class Input>
bool fill_coredump_metadata(Input& input, const CoredumpHeader& outer,
                           uint16_t machine, uint32_t version,
                           const char* running_hash, size_t running_hash_len,
                           bool checksum_ok, CoredumpMetadata& result) {
    if (!outer.readable || !input.image_ready() || !checksum_ok || outer.version != version ||
        input.version() != version ||
        input.length() != outer.length) return false;
    uint8_t elf[52];
    // Match the fixed serialized regions byte-for-byte. Only machine and phnum vary.
    static constexpr uint8_t prefix[18] = {
        0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0};
    static constexpr uint8_t middle[24] = {
        1, 0, 0, 0, 0, 0, 0, 0, 52, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 52, 0, 32, 0};
    static constexpr uint8_t suffix[6] = {40, 0, 0, 0, 0, 0};
    if (!input.payload(0, elf, sizeof(elf)) ||
        std::memcmp(elf, prefix, sizeof(prefix)) != 0 || dump_u16(elf + 18) != machine ||
        std::memcmp(elf + 20, middle, sizeof(middle)) != 0 ||
        std::memcmp(elf + 46, suffix, sizeof(suffix)) != 0) return false;
    result.headers = dump_u16(elf + 44);
    if (!result.headers || result.headers > kCoredumpMaxHeaders) return false;
    const uint32_t end = outer.length - 12 - 32;
    uint32_t next_data = 52 + uint32_t{result.headers} * 32;
    if (next_data > end) return false;
    unsigned info_count = 0, extra_count = 0, notes = 0;
    size_t hash_len = 0;
    for (uint16_t i = 0; i < result.headers; ++i) {
        DumpProgramHeader ph;
        if (!dump_program_header(input, i, ph) || (ph.type != 1 && ph.type != 4) ||
            !ph.bytes || ph.bytes % 4 != 0 || ph.offset != next_data || ph.bytes != ph.memory_bytes ||
            !dump_range(ph.offset, ph.bytes, end) ||
            (ph.type == 1 && ph.bytes > UINT32_MAX - ph.address)) return false;
        next_data += ph.bytes;
        if (ph.type != 4) continue;
        uint32_t consumed = 0;
        while (consumed < ph.bytes) {
            if (++notes > kCoredumpMaxNotes || ph.bytes - consumed < 12) return false;
            uint8_t note[12];
            if (!input.payload(ph.offset + consumed, note, sizeof(note))) return false;
            const uint32_t name_bytes = dump_u32(note), desc_bytes = dump_u32(note + 4);
            const uint32_t type = dump_u32(note + 8);
            uint32_t padded_name = 0, padded_desc = 0;
            if (!name_bytes || name_bytes > 32 || !dump_align4(name_bytes, padded_name) ||
                !dump_align4(desc_bytes, padded_desc)) return false;
            uint32_t remaining = ph.bytes - consumed - 12;
            if (padded_name > remaining) return false;
            remaining -= padded_name;
            if (padded_desc > remaining) return false;
            char name[32]{};
            if (!input.payload(ph.offset + consumed + 12, name, name_bytes) ||
                name[name_bytes - 1] != 0) return false;
            const uint32_t desc_offset = ph.offset + consumed + 12 + padded_name;
            const bool info_name = name_bytes == sizeof("ESP_CORE_DUMP_INFO") &&
                                   std::memcmp(name, "ESP_CORE_DUMP_INFO", name_bytes) == 0;
            const bool extra_name = name_bytes == sizeof("ESP_EXTRA_INFO") &&
                                    std::memcmp(name, "ESP_EXTRA_INFO", name_bytes) == 0;
            if (type == 8266 || info_name) {
                if (!info_name || type != 8266 || ++info_count != 1 || desc_bytes != 72) return false;
                uint8_t info[72];
                if (!input.payload(desc_offset, info, sizeof(info)) || dump_u32(info) != version) return false;
                hash_len = 0;
                while (hash_len < 66 && info[4 + hash_len] != 0) ++hash_len;
                for (size_t j = 0; j < hash_len; ++j) {
                    if (dump_hex(info[4 + j]) > 15) { hash_len = 0; break; }
                }
                if (hash_len >= 8 && hash_len <= 64) {
                    std::memcpy(result.hash, info + 4, hash_len);
                    result.hash[hash_len] = 0;
                } else {
                    hash_len = 0;
                }
            } else if (type == 677 || extra_name) {
                if (!extra_name || type != 677 || ++extra_count != 1) return false;
                result.extra_offset = desc_offset;
                result.extra_bytes = desc_bytes;
            }
            consumed += 12 + padded_name + padded_desc;
        }
    }
    // The writer adds less than one 32-byte cache block before the checksum.
    if (info_count != 1 || next_data > end || end - next_data >= 32 || input.failed()) return false;
    result.identity = classify_dump_identity(running_hash, running_hash_len,
                                             result.hash, hash_len, checksum_ok);
    result.valid_elf = true;
    return true;
}

template <class Input>
CoredumpMetadata parse_coredump_metadata(Input& input, const CoredumpHeader& outer,
                                        uint16_t machine, uint32_t version,
                                        const char* running_hash, size_t running_hash_len,
                                        bool checksum_ok) {
    CoredumpMetadata result;
    if (!fill_coredump_metadata(input, outer, machine, version, running_hash,
                                running_hash_len, checksum_ok, result)) result = {};
    return result;
}

// The integration shell supplies sizeof/offsetof from the compiled target ABI.
// Host fixtures exercise bounds; they do not establish FreeRTOS/exception layouts.
struct CoredumpAbi {
    bool xtensa{false};
    uint32_t tcb_bytes{0}, name_offset{0}, name_bytes{0};
    uint32_t frame_bytes{0}, pc_offset{0}, sp_offset{0}, a0_offset{0};
    uint32_t extra_bytes{0};
};

inline constexpr uint8_t kCoredumpBacktraceMax = 16;
struct CoredumpSummary {
    char task[17]{};
    uint32_t pc{0};                       // raw exception PC, including faulting zero
    uint32_t backtrace[kCoredumpBacktraceMax]{};
    uint8_t depth{0};
    bool available{false}, corrupted{false}, truncated{false}, isr{false};
};
static_assert(sizeof(CoredumpSummary) <= 96, "bounded diagnostic summary");

inline bool dump_virtual_overlap(const DumpProgramHeader& a, const DumpProgramHeader& b) {
    // Compare distances without overflowing a 32-bit endpoint. This also preserves
    // the strict comparisons for empty ranges and addresses at UINT32_MAX.
    if (a.address < b.address) return b.address - a.address < a.bytes;
    if (b.address < a.address) return a.address - b.address < b.bytes;
    return a.bytes != 0 && b.bytes != 0;
}

// Pinned Xtensa esp_cpu_process_stack_pc semantics, with the subtraction checked.
// Executability is a target-supplied numerical region check, never a PC dereference.
inline bool dump_normalize_xtensa_pc(uint32_t pc, uint32_t& normalized) {
    if (pc & 0x80000000U) pc = (pc & 0x3fffffffU) | 0x40000000U;
    if (pc < 3) return false;
    normalized = pc - 3;
    return true;
}

template <class Input>
bool dump_word(Input& input, uint32_t offset, uint32_t& result) {
    uint8_t bytes[4];
    if (!input.payload(offset, bytes, sizeof(bytes))) return false;
    result = dump_u32(bytes);
    return true;
}

template <bool Xtensa, class Input, class Executable, class Abi>
bool fill_coredump_summary_for_arch(Input& input, const CoredumpMetadata& metadata,
                                   const Abi& abi, Executable& executable,
                                   CoredumpSummary& result) {
    if (abi.xtensa != Xtensa) return false;
    if (input.failed() || !input.image_ready() || !metadata.valid_elf ||
        metadata.identity != DumpIdentity::VerifiedCurrent || !metadata.headers ||
        metadata.headers > kCoredumpMaxHeaders || metadata.extra_bytes != abi.extra_bytes ||
        abi.extra_bytes != (Xtensa ? 152U : 8U) || !abi.tcb_bytes || abi.tcb_bytes > 256 ||
        !abi.name_bytes || abi.name_bytes > 16 ||
        !dump_range(abi.name_offset, abi.name_bytes, abi.tcb_bytes) ||
        !abi.frame_bytes || abi.frame_bytes > 256 ||
        !dump_range(abi.pc_offset, 4, abi.frame_bytes) ||
        (Xtensa && (!dump_range(abi.sp_offset, 4, abi.frame_bytes) ||
                       !dump_range(abi.a0_offset, 4, abi.frame_bytes)))) return false;

    uint32_t crashed_tcb = 0, isr = 0;
    if (!dump_word(input, metadata.extra_offset, crashed_tcb) ||
        !dump_word(input, metadata.extra_offset + abi.extra_bytes - 4, isr) ||
        isr > 1 || !crashed_tcb || crashed_tcb == UINT32_MAX ||
        crashed_tcb == 0xdeadbeefU || crashed_tcb % 4 != 0) return false;

    DumpProgramHeader tcb, stack;
    uint16_t tcb_index = 0;
    unsigned matches = 0;
    for (uint16_t i = 0; i < metadata.headers; ++i) {
        DumpProgramHeader ph;
        if (!dump_program_header(input, i, ph)) return false;
        if (ph.type == 1 && ph.address == crashed_tcb) {
            tcb = ph;
            tcb_index = i;
            ++matches;
        }
    }
    if (matches != 1 || tcb_index + 1 >= metadata.headers || tcb.bytes < abi.tcb_bytes ||
        !dump_program_header(input, tcb_index + 1, stack) || stack.type != 1 ||
        stack.address % 4 != 0 || stack.bytes < abi.frame_bytes ||
        tcb.bytes > UINT32_MAX - tcb.address || stack.bytes > UINT32_MAX - stack.address ||
        dump_virtual_overlap(tcb, stack)) return false;

    const DumpProgramHeader fake_stack{1, 0, Xtensa ? 0x20000000U : 0x70000000U,
                                      0x10000000U, 0x10000000U};
    if (dump_virtual_overlap(stack, fake_stack)) return false;
    // A constant number of linear passes; never pairwise O(phnum^2) overlap scans.
    for (uint16_t i = 0; i < metadata.headers; ++i) {
        DumpProgramHeader ph;
        if (!dump_program_header(input, i, ph)) return false;
        if (ph.type != 1 || i == tcb_index || i == tcb_index + 1) continue;
        if (ph.bytes > UINT32_MAX - ph.address || dump_virtual_overlap(ph, tcb) ||
            dump_virtual_overlap(ph, stack)) return false;
    }

    if (!input.payload(tcb.offset + abi.name_offset, result.task, abi.name_bytes) ||
        !dump_word(input, stack.offset + abi.pc_offset, result.pc)) return false;
    for (unsigned i = 0; i < abi.name_bytes && result.task[i]; ++i) {
        const auto c = static_cast<unsigned char>(result.task[i]);
        if (c < 0x20 || c > 0x7e) result.task[i] = '?';
    }
    result.available = true;
    result.isr = isr != 0;
    if constexpr (!Xtensa) {
        result.corrupted = result.pc == 0 || (result.pc & 1) != 0 || !executable(result.pc);
        return true;                    // no RISC-V stack copy or speculative unwind
    }

    uint32_t pc = 0;
    if (!dump_normalize_xtensa_pc(result.pc, pc) || !executable(pc)) {
        result.corrupted = true;
        return true;                    // keep raw fault context without following it
    }
    result.backtrace[result.depth++] = pc;
    uint32_t sp = 0, next_pc = 0;
    if (!dump_word(input, stack.offset + abi.sp_offset, sp) ||
        !dump_word(input, stack.offset + abi.a0_offset, next_pc)) return false;
    const auto valid_sp = [&](uint32_t value) {
        return value % 16 == 0 && value >= stack.address &&
               value - stack.address >= 16 && value - stack.address <= stack.bytes;
    };
    while (next_pc != 0) {
        if (!valid_sp(sp) || !dump_normalize_xtensa_pc(next_pc, pc) || !executable(pc)) {
            result.corrupted = true;
            break;
        }
        result.backtrace[result.depth++] = pc;
        uint8_t link[8];
        if (!input.payload(stack.offset + (sp - stack.address) - 16, link, sizeof(link))) return false;
        next_pc = dump_u32(link);
        const uint32_t next_sp = dump_u32(link + 4);
        if (result.depth == kCoredumpBacktraceMax) {
            result.truncated = next_pc != 0;
            break;                       // no seventeenth PC, window or read
        }
        if (next_pc && (next_sp <= sp || !valid_sp(next_sp))) {
            result.corrupted = true;
            break;
        }
        sp = next_sp;
    }
    return true;
}

template <bool Xtensa, class Input, class Executable, class Abi>
CoredumpSummary parse_coredump_summary_for_arch(Input& input, const CoredumpMetadata& metadata,
                                       const Abi& abi, Executable executable) {
    CoredumpSummary result;
    if (!fill_coredump_summary_for_arch<Xtensa>(input, metadata, abi, executable, result))
        result = {};
    return result;
}

template <class Input, class Executable>
CoredumpSummary parse_coredump_summary(Input& input, const CoredumpMetadata& metadata,
                                      const CoredumpAbi& abi, Executable executable) {
    return abi.xtensa ? parse_coredump_summary_for_arch<true>(input, metadata, abi, executable)
                      : parse_coredump_summary_for_arch<false>(input, metadata, abi, executable);
}

}  // namespace tk
