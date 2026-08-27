#pragma once

// Deterministic ownership shell for the temporary MQTT broker probe. Ops deliberately supplies
// only C-handle types and non-throwing cleanup calls, which makes every partial-acquire stage
// host-testable without linking ESP-MQTT or FreeRTOS.
namespace tk {

template <typename Ops>
class MqttProbeResourceOwner {
public:
    using Client = typename Ops::Client;
    using Semaphore = typename Ops::Semaphore;

    Client    client{};
    Semaphore sem{};
    bool      started{false};

    MqttProbeResourceOwner() = default;
    MqttProbeResourceOwner(const MqttProbeResourceOwner&) = delete;
    MqttProbeResourceOwner& operator=(const MqttProbeResourceOwner&) = delete;

    ~MqttProbeResourceOwner() noexcept {
        // stop() joins the MQTT task, so no callback can touch its context before the client and
        // semaphore are destroyed. Every member is optional to cover partial acquisition.
        if (client && started) Ops::stop(client);
        if (client) Ops::destroy(client);
        if (sem) Ops::delete_semaphore(sem);
    }
};

}  // namespace tk
