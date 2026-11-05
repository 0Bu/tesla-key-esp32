#pragma once
#include <cstddef>
#include "esp_ota_ops.h"
struct httpd_req { void* handle; std::size_t content_len; };
using httpd_req_t = httpd_req;
esp_err_t httpd_req_async_handler_begin(httpd_req_t*, httpd_req_t**);
esp_err_t httpd_req_async_handler_complete(httpd_req_t*);
int httpd_req_to_sockfd(httpd_req_t*);
int httpd_req_recv(httpd_req_t*, char*, std::size_t);
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*);
esp_err_t httpd_resp_sendstr(httpd_req_t*, const char*);
