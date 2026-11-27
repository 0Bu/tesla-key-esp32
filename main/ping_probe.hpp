#pragma once

#include "logic/ping_probe.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"

#include <cstdint>

namespace tk {

struct PingProbeControl;

struct PingProbeCallbackArgs {
    PingProbeControl* control{nullptr};
    std::uint32_t generation{0};
    std::uint32_t expected_requests{0};
};

// Persistent owner. `done`, callback args and generation state outlive every esp_ping task.
// The generation in callback_args binds the handle to its exact on_ping_end through cleanup.
struct PingProbeControl {
    SemaphoreHandle_t done{nullptr};
    PingProbeGeneration generation{};
    PingProbeCallbackArgs callback_args{};
    esp_ping_handle_t session{nullptr};
    bool session_started{false};
};

enum class PingProbeResult : std::uint8_t {
    Reply,
    NoReply,
    Unavailable,
    PendingEnd,
};

inline void ping_probe_on_end(esp_ping_handle_t handle, void* raw_args) noexcept {
    auto* args = static_cast<PingProbeCallbackArgs*>(raw_args);
    if (!args || !args->control || args->generation == 0) return;
    std::uint32_t replies = 0;
    std::uint32_t sent = 0;
    const bool have_reply_count =
        esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &replies, sizeof(replies)) == ESP_OK;
    // The SDK can finish after failing to transmit. Its request counter includes only
    // successful sends, so read it before treating a zero-reply batch as evidence.
    const bool have_counts = have_reply_count && (replies != 0 ||
        esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &sent, sizeof(sent)) == ESP_OK);
    const bool measurement_valid = have_counts &&
        ping_probe_measurement_valid(args->expected_requests, sent, replies);
    PingProbeControl& control = *args->control;
    if (control.generation.complete(args->generation, replies, measurement_valid) && control.done) {
        xSemaphoreGive(control.done);
    }
}

inline bool ping_probe_wait_for_end(PingProbeControl& control, std::uint32_t generation,
                                    TickType_t timeout) {
    if (control.generation.ended(generation)) return true;
    const TickType_t started = xTaskGetTickCount();
    TickType_t remaining = timeout;
    for (;;) {
        if (xSemaphoreTake(control.done, remaining) != pdTRUE) {
            return control.generation.ended(generation);
        }
        if (control.generation.ended(generation)) return true;
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) return false;
        remaining = timeout - elapsed;
    }
}

inline bool ping_probe_cleanup_completed(PingProbeControl& control) {
    if (!control.session) return true;
    const std::uint32_t generation = control.callback_args.generation;
    // A session whose start failed cannot produce on_ping_end. If its immediate delete also
    // failed, retain the exact handle and generation and retry deletion before allocating any
    // replacement. Dropping them here would leak one SDK session on every watchdog/probe retry.
    if (!control.session_started) {
        if (esp_ping_delete_session(control.session) != ESP_OK) return false;
        const bool released = control.generation.abandon_unstarted(generation);
        control.session = nullptr;
        control.callback_args = {};
        // Even an impossible ownership mismatch must not retain an already-deleted SDK handle
        // and double-delete it on the next pass. Leave the generation fail-closed instead.
        return released;
    }
    if (!control.generation.ended(generation)) return false;
    if (esp_ping_delete_session(control.session) != ESP_OK) return false;
    const bool released = control.generation.retire(generation);
    control.session = nullptr;
    control.session_started = false;
    control.callback_args = {};
    return released;
}

// Runs one session or returns PendingEnd while a stopped session still owes its exact callback.
// Crucially, PendingEnd retains the handle/control block and prevents all generation reuse.
inline PingProbeResult ping_probe_run(PingProbeControl& control,
                                      const esp_ping_config_t& config,
                                      TickType_t completion_timeout,
                                      TickType_t stop_ack_timeout,
                                      void (*on_end)(esp_ping_handle_t, void*) = ping_probe_on_end) {
    if (!control.done || !on_end) return PingProbeResult::Unavailable;
    if (!ping_probe_cleanup_completed(control)) return PingProbeResult::PendingEnd;

    while (xSemaphoreTake(control.done, 0) == pdTRUE) {
        // Drain only before a new generation; accepted callbacks are generation-bound below.
    }
    const std::uint32_t generation = control.generation.begin();
    if (generation == 0) return PingProbeResult::PendingEnd;

    control.callback_args = {&control, generation, config.count};
    esp_ping_callbacks_t callbacks{};
    callbacks.cb_args = &control.callback_args;
    callbacks.on_ping_end = on_end;

    esp_ping_handle_t session = nullptr;
    if (esp_ping_new_session(&config, &callbacks, &session) != ESP_OK || !session) {
        control.callback_args = {};
        control.generation.abandon_unstarted(generation);
        return PingProbeResult::Unavailable;
    }
    control.session = session;
    control.session_started = false;
    if (esp_ping_start(session) != ESP_OK) {
        // No ping task started, so on_ping_end is impossible and direct cleanup is safe.
        // Preserve ownership when deletion itself fails; the next serial caller retries exact
        // cleanup and cannot begin a new generation in the meantime.
        if (esp_ping_delete_session(session) == ESP_OK) {
            control.session = nullptr;
            control.callback_args = {};
            control.generation.abandon_unstarted(generation);
        }
        return PingProbeResult::Unavailable;
    }
    control.session_started = true;

    bool ended = ping_probe_wait_for_end(control, generation, completion_timeout);
    if (!ended) {
        esp_ping_stop(session);
        ended = ping_probe_wait_for_end(control, generation, stop_ack_timeout);
    }
    if (!ended) {
        // Do not delete or reuse anything. A late on_ping_end safely lands in this persistent
        // generation and a later serial call performs the exact cleanup.
        return PingProbeResult::PendingEnd;
    }

    std::uint32_t replies = 0;
    bool measurement_valid = false;
    if (!control.generation.result(generation, replies, measurement_valid) ||
        !ping_probe_cleanup_completed(control)) {
        return PingProbeResult::PendingEnd;
    }
    if (!measurement_valid) return PingProbeResult::Unavailable;
    return replies > 0 ? PingProbeResult::Reply : PingProbeResult::NoReply;
}

}  // namespace tk
