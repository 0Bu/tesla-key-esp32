#pragma once
#include <cstddef>
#include <sys/types.h>
struct esp_tls { int id; };
using esp_tls_t = esp_tls;
struct esp_tls_cfg_t {
    bool non_block{false};
    int timeout_ms{0};
    const char* common_name{nullptr};
    bool skip_common_name{false};
    int (*crt_bundle_attach)(void*){nullptr};
};
esp_tls_t* esp_tls_init();
int esp_tls_conn_new_sync(const char*,int,int,const esp_tls_cfg_t*,esp_tls_t*);
ssize_t esp_tls_conn_write(esp_tls_t*,const void*,std::size_t);
ssize_t esp_tls_conn_read(esp_tls_t*,void*,std::size_t);
int esp_tls_conn_destroy(esp_tls_t*);
