#pragma once

#include "vin.hpp"

#include <string>

// Pure, hardware-free Home Assistant identity logic. The node id is part of every MQTT state
// topic, discovery topic, entity unique_id and device identifier, so it must describe the VEHICLE,
// not the replaceable ESP32 board. Keeping this beside the VIN validator makes the contract
// host-testable without ESP-IDF, NVS, cJSON or MQTT.
namespace tk {

inline std::string ha_node_id_from_vin(const std::string& vin) {
    if (!vin_is_plausible(vin)) return {};

    std::string node = "teslakey_";
    node.reserve(node.size() + vin.size());
    for (char c : vin) {
        node.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return node;
}

}  // namespace tk
