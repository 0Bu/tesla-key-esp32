#pragma once

#include "json_builder.hpp"

#include <cJSON.h>

#include <memory>
#include <utility>

namespace tk {

struct MqttJsonStringDelete {
    void operator()(char* value) const noexcept { cJSON_free(value); }
};

using MqttJsonStringOwner = std::unique_ptr<char, MqttJsonStringDelete>;

// Build/print and transport are deliberately separated at this seam: a retained MQTT topic is
// never touched until cJSON has produced the complete payload. JsonOwner keeps the full tree alive
// through printing and frees it on every builder/print/publish failure. The transport callback
// returns true only when the broker client accepted the publish.
template <typename Publish>
bool mqtt_publish_json(const char* topic,
                       JsonOwner root,
                       bool retain,
                       Publish&& publish) noexcept {
    if (!topic || !root) return false;

    MqttJsonStringOwner payload(cJSON_PrintUnformatted(root.get()));
    if (!payload) return false;

    try {
        return static_cast<bool>(
            std::forward<Publish>(publish)(topic, payload.get(), retain));
    } catch (...) {
        return false;
    }
}

}  // namespace tk
