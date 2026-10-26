#pragma once
#include "nvs.h"
#include <cstddef>
struct HostPingSession;
using esp_ping_handle_t=HostPingSession*;
struct ip_addr_t { unsigned addr{}; };
struct esp_ping_config_t {
    ip_addr_t target_addr{};
    unsigned count{}, timeout_ms{}, interval_ms{}, interface{};
};
struct esp_ping_callbacks_t {
    void (*on_ping_success)(esp_ping_handle_t,void*){};
    void (*on_ping_timeout)(esp_ping_handle_t,void*){};
    void (*on_ping_end)(esp_ping_handle_t,void*){};
    void* cb_args{};
};
inline constexpr int ESP_PING_PROF_REPLY=1;
#define ESP_PING_DEFAULT_CONFIG() esp_ping_config_t{}
extern "C" {
esp_err_t esp_ping_new_session(const esp_ping_config_t*,const esp_ping_callbacks_t*,esp_ping_handle_t*);
esp_err_t esp_ping_start(esp_ping_handle_t);
esp_err_t esp_ping_stop(esp_ping_handle_t);
esp_err_t esp_ping_delete_session(esp_ping_handle_t);
esp_err_t esp_ping_get_profile(esp_ping_handle_t,int,void*,std::size_t);
}
