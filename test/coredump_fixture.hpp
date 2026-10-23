#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

// Synthetic serialized IDF writer fixtures; no real dump or secret data.
static void dump_test_u32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<uint8_t>(value >> (i * 8));
}

static std::vector<uint8_t> dump_test_note(const char* name, uint32_t type,
                                           const std::vector<uint8_t>& description) {
    const size_t length = std::strlen(name) + 1;
    std::vector<uint8_t> bytes(12, 0);
    dump_test_u32(bytes, 0, static_cast<uint32_t>(length));
    dump_test_u32(bytes, 4, static_cast<uint32_t>(description.size()));
    dump_test_u32(bytes, 8, type);
    bytes.insert(bytes.end(), name, name + length);
    while (bytes.size() % 4) bytes.push_back(0);
    bytes.insert(bytes.end(), description.begin(), description.end());
    while (bytes.size() % 4) bytes.push_back(0);
    return bytes;
}

[[maybe_unused]] static std::vector<uint8_t> dump_test_image(const std::vector<uint8_t>& notes,
                                            uint16_t headers = 1) {
    std::vector<uint8_t> bytes(12 + 52 + 32 * headers, 0);
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    std::memcpy(bytes.data() + 12, ident, sizeof(ident));
    bytes[12 + 16] = 4;       // ET_CORE
    bytes[12 + 18] = 94;      // EM_XTENSA
    bytes[12 + 20] = 1;
    bytes[12 + 28] = 52;
    bytes[12 + 40] = 52;
    bytes[12 + 42] = 32;
    bytes[12 + 44] = static_cast<uint8_t>(headers);
    bytes[12 + 45] = static_cast<uint8_t>(headers >> 8);
    bytes[12 + 46] = 40;
    uint32_t offset = 52 + 32 * uint32_t{headers};
    for (uint16_t i = 0; i < headers; ++i) {
        const size_t ph = 12 + 52 + 32 * size_t{i};
        const uint32_t length = i == 0 ? static_cast<uint32_t>(notes.size()) : 4;
        dump_test_u32(bytes, ph, i == 0 ? 4 : 1);
        dump_test_u32(bytes, ph + 4, offset);
        const uint32_t address = i == 0 ? 0 : 0x3ffb0000 + uint32_t{i} * 16;
        dump_test_u32(bytes, ph + 8, address);
        dump_test_u32(bytes, ph + 12, address);
        dump_test_u32(bytes, ph + 16, length);
        dump_test_u32(bytes, ph + 20, length);
        dump_test_u32(bytes, ph + 24, 6);  // PF_R | PF_W, including the writer's PT_NOTE
        offset += length;
    }
    bytes.insert(bytes.end(), notes.begin(), notes.end());
    bytes.resize(bytes.size() + (headers - 1) * 4, 0);
    while (bytes.size() % 32) bytes.push_back(0);
    bytes.resize(bytes.size() + 32, 0);  // metadata tests supply integrity independently
    dump_test_u32(bytes, 0, static_cast<uint32_t>(bytes.size()));
    dump_test_u32(bytes, 4, 0x00090104);
    return bytes;
}

static std::vector<uint8_t> dump_test_summary_image(bool xtensa, uint32_t version,
                                                    unsigned depth = 1,
                                                    bool remaining = false,
                                                    unsigned extra_loads = 0) {
    const uint32_t tcb_address = 0x3ffb0000, stack_address = 0x3ffb1000;
    std::vector<uint8_t> info(72), extra(xtensa ? 152 : 8);
    dump_test_u32(info, 0, version);
    std::fill(info.begin() + 4, info.begin() + 68, 'a');
    dump_test_u32(extra, 0, tcb_address);
    auto notes = dump_test_note("ESP_CORE_DUMP_INFO", 8266, info);
    const auto extra_note = dump_test_note("ESP_EXTRA_INFO", 677, extra);
    notes.insert(notes.end(), extra_note.begin(), extra_note.end());
    std::vector<std::vector<uint8_t>> segments{notes, std::vector<uint8_t>(xtensa ? 100 : 96),
                                             std::vector<uint8_t>(xtensa ? 1024 : 148)};
    std::memcpy(segments[1].data() + 52, "vehicle_loop", 12);
    dump_test_u32(segments[2], xtensa ? 4 : 0, xtensa ? 0x40001003 : 0x42001000);
    if (xtensa) {
        dump_test_u32(segments[2], 16, stack_address + 128);
        dump_test_u32(segments[2], 12, depth > 1 ? 0x80002003 : 0);
        for (unsigned frame = 2; frame <= depth; ++frame) {
            const unsigned window = 128 + (frame - 2) * 16 - 16;
            dump_test_u32(segments[2], window,
                          frame < depth || remaining ? 0x80002003 + (frame - 1) * 16 : 0);
            dump_test_u32(segments[2], window + 4, stack_address + 128 + (frame - 1) * 16);
        }
    }
    for (unsigned i = 0; i < extra_loads; ++i) segments.emplace_back(4, 0);
    const uint16_t count = static_cast<uint16_t>(segments.size());
    std::vector<uint8_t> bytes(12 + 52 + 32 * count);
    const uint8_t ident[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    std::memcpy(bytes.data() + 12, ident, sizeof(ident));
    bytes[12 + 16] = 4;
    bytes[12 + 18] = xtensa ? 94 : 243;
    bytes[12 + 20] = 1;
    bytes[12 + 28] = 52;
    bytes[12 + 40] = 52;
    bytes[12 + 42] = 32;
    bytes[12 + 44] = static_cast<uint8_t>(count);
    bytes[12 + 45] = static_cast<uint8_t>(count >> 8);
    bytes[12 + 46] = 40;
    uint32_t offset = 52 + 32 * count;
    for (unsigned i = 0; i < segments.size(); ++i) {
        const size_t ph = 12 + 52 + 32 * i;
        const uint32_t address = i == 0 ? 0 : i == 1 ? tcb_address : i == 2 ? stack_address
                                                                                       : 0x3ffc0000 + i * 16;
        dump_test_u32(bytes, ph, i == 0 ? 4 : 1);
        dump_test_u32(bytes, ph + 4, offset);
        dump_test_u32(bytes, ph + 8, address);
        dump_test_u32(bytes, ph + 12, address);
        dump_test_u32(bytes, ph + 16, static_cast<uint32_t>(segments[i].size()));
        dump_test_u32(bytes, ph + 20, static_cast<uint32_t>(segments[i].size()));
        dump_test_u32(bytes, ph + 24, 6);
        offset += static_cast<uint32_t>(segments[i].size());
    }
    for (const auto& segment : segments) bytes.insert(bytes.end(), segment.begin(), segment.end());
    while (bytes.size() % 32) bytes.push_back(0);
    bytes.resize(bytes.size() + 32);
    dump_test_u32(bytes, 0, static_cast<uint32_t>(bytes.size()));
    dump_test_u32(bytes, 4, version);
    return bytes;
}
