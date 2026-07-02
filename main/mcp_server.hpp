#pragma once

#include "vehicle_ctrl.hpp"
#include <esp_http_server.h>

// MCP (Model Context Protocol) endpoint — Streamable HTTP transport, STATELESS:
// POST /mcp carries a single JSON-RPC 2.0 message and is answered with application/json
// (no SSE stream, no Mcp-Session-Id). Dispatched from http_server.cpp's catch-all, so
// both handlers run under its handle_all try/catch (OOM → 503, never a reboot).
esp_err_t mcp_handle_post(httpd_req_t* req, VehicleController& vehicle);

// GET /mcp — this server offers no server-initiated SSE stream: 405 with Allow: POST.
esp_err_t mcp_handle_get(httpd_req_t* req);
