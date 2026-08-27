#pragma once

#include "nvs.h"

#include <cstddef>

struct HostPingSession;
using esp_ping_handle_t = HostPingSession*;

struct esp_ping_config_t {
    unsigned count{0};
};

struct esp_ping_callbacks_t {
    void (*on_ping_success)(esp_ping_handle_t, void*){nullptr};
    void (*on_ping_timeout)(esp_ping_handle_t, void*){nullptr};
    void (*on_ping_end)(esp_ping_handle_t, void*){nullptr};
    void* cb_args{nullptr};
};

inline constexpr int ESP_PING_PROF_REPLY = 1;

extern "C" {
esp_err_t esp_ping_new_session(const esp_ping_config_t* config,
                               const esp_ping_callbacks_t* callbacks,
                               esp_ping_handle_t* out_session);
esp_err_t esp_ping_start(esp_ping_handle_t session);
esp_err_t esp_ping_stop(esp_ping_handle_t session);
esp_err_t esp_ping_delete_session(esp_ping_handle_t session);
esp_err_t esp_ping_get_profile(esp_ping_handle_t session, int profile,
                               void* value, std::size_t value_size);
}
