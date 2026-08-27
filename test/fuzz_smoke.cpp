#include "logic/config_store.hpp"
#include "logic/http_body.hpp"
#include "logic/json_syntax.hpp"
#include "logic/mqtt_uri.hpp"
#include "logic/redact.hpp"
#include "logic/syslog_policy.hpp"
#include "logic/vin_transition.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const char* invariant, std::uint64_t seed, std::size_t iteration) {
    std::cerr << "fuzz-smoke: invariant failed: " << invariant << " seed=" << seed
              << " iteration=" << iteration << '\n';
    std::exit(1);
}

struct Rng {
    std::uint64_t state;

    std::uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * UINT64_C(2685821657736338717);
    }

    std::size_t bounded(std::size_t bound) {
        return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
    }
};

std::string random_string(Rng& rng, std::size_t max_len) {
    std::string value(rng.bounded(max_len + 1), '\0');
    for (char& c : value) c = static_cast<char>(rng.next() & 0xffu);
    return value;
}

bool same_config(const tk::ConfigBlob& a, const tk::ConfigBlob& b) {
    return a.wifi_ssid == b.wifi_ssid && a.wifi_pass == b.wifi_pass &&
           a.wifi_ssid_backup == b.wifi_ssid_backup &&
           a.wifi_pass_backup == b.wifi_pass_backup &&
           a.wifi_rollback_active == b.wifi_rollback_active &&
           a.wifi_rolled_back == b.wifi_rolled_back && a.vin == b.vin &&
           a.mqtt_uri == b.mqtt_uri && a.syslog_uri == b.syslog_uri;
}

std::uint64_t parse_u64(const char* value, const char* label) {
    if (!value || !*value) {
        std::cerr << "fuzz-smoke: missing " << label << '\n';
        std::exit(2);
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    if (!end || *end != '\0') {
        std::cerr << "fuzz-smoke: invalid " << label << ": " << value << '\n';
        std::exit(2);
    }
    return static_cast<std::uint64_t>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = UINT64_C(0x5445534c414b4559);
    std::size_t iterations = 20000;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            seed = parse_u64(argv[++i], "seed");
        } else if (arg == "--iterations" && i + 1 < argc) {
            const std::uint64_t parsed = parse_u64(argv[++i], "iterations");
            if (parsed == 0 || parsed > 100000) {
                std::cerr << "fuzz-smoke: iterations must be in 1..100000\n";
                return 2;
            }
            iterations = static_cast<std::size_t>(parsed);
        } else {
            std::cerr << "usage: fuzz_smoke [--seed N] [--iterations 1..100000]\n";
            return 2;
        }
    }

    Rng rng{seed == 0 ? UINT64_C(0x9e3779b97f4a7c15) : seed};
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const std::string bytes = random_string(rng, 512);

        const bool json_valid = tk::json_syntax_valid(bytes.data(), bytes.size());
        std::string json_with_ignored_suffix = bytes;
        json_with_ignored_suffix.push_back(static_cast<char>(rng.next() & 0xffu));
        if (json_valid != tk::json_syntax_valid(bytes.data(), bytes.size()) ||
            json_valid != tk::json_syntax_valid(json_with_ignored_suffix.data(), bytes.size()))
            fail("JSON syntax validation is deterministic and length bounded", seed, iteration);

        const std::string trimmed = tk::mqtt_trim(bytes);
        if (trimmed.size() > bytes.size() || tk::mqtt_trim(trimmed) != trimmed)
            fail("mqtt trim is bounded and idempotent", seed, iteration);
        const std::string effective = tk::mqtt_effective_uri(bytes, (rng.next() & 1u) != 0);
        if (effective.size() > trimmed.size() + 8u ||
            (trimmed.empty() && !effective.empty()))
            fail("effective MQTT URI is bounded", seed, iteration);
        if (tk::mqtt_broker_is_plausible(bytes) != tk::mqtt_broker_is_plausible(bytes))
            fail("MQTT plausibility is deterministic", seed, iteration);
        if (tk::syslog_target_is_plausible(bytes) != tk::syslog_target_is_plausible(bytes))
            fail("syslog plausibility is deterministic", seed, iteration);

        const std::string redacted = tk::redact_diag_line(bytes);
        if (tk::redact_diag_line(redacted) != redacted ||
            redacted.size() > bytes.size() + tk::kDiagRedactionCount * std::strlen(tk::kRedacted))
            fail("diagnostic redaction is bounded and idempotent", seed, iteration);

        tk::VinTransitionMarker marker{"sentinel-vin", "AA:BB:CC:DD"};
        const tk::VinTransitionMarker before = marker;
        const bool parsed_marker = tk::parse_vin_transition_marker(bytes, marker);
        if (!parsed_marker && (marker.previous_vin != before.previous_vin ||
                               marker.previous_key_id != before.previous_key_id))
            fail("failed VIN journal parse is non-mutating", seed, iteration);
        const std::string vin = random_string(rng, 40);
        const std::string key = (rng.next() & 1u) != 0 ? "0A:1B:2C:3D" : "";
        const std::string encoded_marker = tk::make_vin_transition_marker(vin, key);
        tk::VinTransitionMarker round_trip_marker;
        if (encoded_marker.empty() ||
            !tk::parse_vin_transition_marker(encoded_marker, round_trip_marker) ||
            round_trip_marker.previous_vin != vin || round_trip_marker.previous_key_id != key)
            fail("VIN journal round trip", seed, iteration);

        std::vector<std::uint8_t> raw(bytes.begin(), bytes.end());
        tk::ConfigBlob output;
        output.wifi_ssid = "sentinel";
        output.wifi_pass = "keep";
        output.wifi_rollback_active = true;
        const tk::ConfigBlob output_before = output;
        const bool decoded = tk::config_blob_decode(raw.empty() ? nullptr : raw.data(), raw.size(), output);
        if (!decoded && !same_config(output, output_before))
            fail("failed config decode is non-mutating", seed, iteration);

        tk::ConfigBlob config;
        config.wifi_ssid = random_string(rng, tk::kConfigMaxSsidLen);
        config.wifi_pass = random_string(rng, tk::kConfigMaxWifiPassLen);
        config.wifi_ssid_backup = random_string(rng, tk::kConfigMaxSsidLen);
        config.wifi_pass_backup = random_string(rng, tk::kConfigMaxWifiPassLen);
        config.wifi_rollback_active = (rng.next() & 1u) != 0;
        config.wifi_rolled_back = (rng.next() & 1u) != 0;
        config.vin = random_string(rng, tk::kConfigMaxVinLen);
        config.mqtt_uri = random_string(rng, tk::kConfigMaxUriLen);
        config.syslog_uri = random_string(rng, tk::kConfigMaxUriLen);
        tk::ConfigBlobBuffer blob{};
        const std::size_t blob_len = tk::config_blob_encode(config, blob.data(), blob.size());
        tk::ConfigBlob decoded_config;
        if (blob_len == 0 || !tk::config_blob_decode(blob.data(), blob_len, decoded_config) ||
            !same_config(config, decoded_config))
            fail("config blob round trip", seed, iteration);

        const std::size_t body_len = 1 + rng.bounded(300);
        const std::string body = random_string(rng, body_len - 1) + "x";
        std::vector<char> received(body_len + 1, static_cast<char>(0x5a));
        std::size_t offset = 0;
        int timeout_budget = static_cast<int>(rng.bounded(tk::BODY_MAX_IDLE + 1));
        const int body_result = tk::http_body_read(
            received.data(), received.size(), body.size(),
            [&](char* destination, std::size_t requested) -> tk::BodyChunk {
                if (timeout_budget-- > 0) return {tk::BodyRecv::Timeout, 0};
                const std::size_t chunk = 1 + rng.bounded(requested);
                std::memcpy(destination, body.data() + offset, chunk);
                offset += chunk;
                return {tk::BodyRecv::Data, chunk};
            });
        if (body_result != static_cast<int>(body.size()) ||
            std::string(received.data(), body.size()) != body || received[body.size()] != '\0')
            fail("segmented HTTP body round trip", seed, iteration);

        std::array<char, 8> guarded{};
        if (tk::http_body_read(guarded.data(), guarded.size(), 4,
                               [](char*, std::size_t requested) -> tk::BodyChunk {
                                   return {tk::BodyRecv::Data, requested + 1};
                               }) != -1)
            fail("oversized recv report is rejected", seed, iteration);
    }

    std::cout << "fuzz-smoke: PASS seed=" << seed << " iterations=" << iterations << '\n';
    return 0;
}
