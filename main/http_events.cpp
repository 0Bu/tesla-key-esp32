// /events — the web UI's live status stream over a WebSocket. The browser opens ONE ws://
// connection, sends the text "sub", and the device pushes the /status JSON (build_status_object(),
// http_status.cpp) — first an immediate snapshot, then a fresh copy every WS_BROADCAST_INTERVAL_MS
// from a background task. This REPLACES the old browser-side interval poll of GET /status; there is
// no poll fallback (see app.js boot()). GET /status itself stays as the request/response form for
// curl and the post-OTA reboot probe.
//
// Registration is deliberately OUTSIDE the GuardedReq/handle_all trampoline: the WS handshake needs
// the raw esp_http_server handler signature (is_websocket=true), which the GuardedReq type-guard in
// http_handlers.hpp exists to forbid for normal routes. So this ONE handler is registered directly
// and takes responsibility for its own OOM discipline: every JSON build here (and in the broadcast
// task, which has no guard above it at all) is wrapped so a std::bad_alloc on this memory-tight chip
// drops a single frame rather than unwinding through httpd's C frames → std::terminate → reboot.
//
// The frame-length/command policy (what an arriving frame MEANS before its payload is touched) is
// the pure, host-tested logic/ws_policy.hpp — the cases that matter (an oversized, undrainable
// frame; a read that failed) are exactly the ones a browser never produces and a test can.
#include "http_handlers.hpp"
#include "logic/ws_policy.hpp"
#include "task_config.hpp"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdlib>      // free() — matches cJSON_PrintUnformatted's allocator (see http_common.cpp)
#include <new>          // std::nothrow, std::bad_alloc
#include <string>

static const char* TAG = "http_events";

// How often the broadcast task pushes a fresh /status frame to every subscriber. Snappier than the
// old 4 s HTTP poll, and a WS push carries none of the per-request TCP/HTTP setup+teardown the poll
// paid — so net churn is comparable while feeling live. The task self-gates on ws_any_clients(), so
// with no browser open (the steady state — evcc talks to /api, not the UI) it costs nothing.
static constexpr uint32_t WS_BROADCAST_INTERVAL_MS = 2000;

// The broadcast list. Capped at 8; httpd's own max_open_sockets (7) keeps that out of reach, but a
// subscriber silently never served looks exactly like a dead link from the browser, so a full list
// is logged rather than dropped without a trace. Guarded by s_ws_mtx (the /events handler runs on
// an httpd session task; the broadcaster + close_fn run on other tasks).
static constexpr int WS_MAX_CLIENTS = 8;
static SemaphoreHandle_t s_ws_mtx = nullptr;
static int s_ws_fds[WS_MAX_CLIENTS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
// Send bookkeeping, one slot per s_ws_fds entry: the backpressure that stops a single non-reading
// subscriber from queueing the heap away (logic/ws_policy.hpp carries the full incident note).
static tk::WsClientSend s_ws_send[WS_MAX_CLIENTS] = {};
static httpd_handle_t s_server = nullptr;

// Add fd to the broadcast list (idempotent). false only when all slots are taken.
static bool ws_register_client(int fd) {
    if (!s_ws_mtx) return false;
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    bool registered = false;
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fds[i] == fd) { registered = true; break; }
    if (!registered)
        for (int i = 0; i < WS_MAX_CLIENTS; i++)
            if (s_ws_fds[i] == -1) {
                s_ws_fds[i]  = fd;
                s_ws_send[i] = {};   // a reused fd starts with a clean in-flight/failure count
                registered = true;
                break;
            }
    xSemaphoreGive(s_ws_mtx);
    return registered;
}

void http_events_on_close(int sockfd) {
    if (!s_ws_mtx) return;
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fds[i] == sockfd) { s_ws_fds[i] = -1; s_ws_send[i] = {}; break; }
    xSemaphoreGive(s_ws_mtx);
}

// Claim an in-flight slot for fd. False when the client is at WS_MAX_INFLIGHT — it is not draining,
// so this tick is skipped for it. Also false for an fd that is no longer registered.
static bool ws_reserve_inflight(int fd) {
    if (!s_ws_mtx) return false;
    bool ok = false;
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fds[i] == fd) {
            if (tk::ws_may_send(s_ws_send[i])) { tk::ws_note_queued(s_ws_send[i]); ok = true; }
            break;
        }
    xSemaphoreGive(s_ws_mtx);
    return ok;
}

// Release a slot claimed by ws_reserve_inflight because the frame actually completed (or failed to
// send). Returns true when this client has now failed WS_MAX_FAILS sends in a row and should be
// closed. An fd already unregistered by the time its completion lands simply matches nothing.
static bool ws_release_inflight(int fd, bool sent_ok) {
    if (!s_ws_mtx) return false;
    bool evict = false;
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fds[i] == fd) { evict = tk::ws_note_completed(s_ws_send[i], sent_ok); break; }
    xSemaphoreGive(s_ws_mtx);
    return evict;
}

// Give back a reservation for a frame WE never managed to hand to httpd. Kept distinct from
// ws_release_inflight(fd, true): our own OOM must not be recorded as a successful send, or it
// would clear the client's failure streak — see ws_note_cancelled().
static void ws_cancel_inflight(int fd) {
    if (!s_ws_mtx) return;
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fds[i] == fd) { tk::ws_note_cancelled(s_ws_send[i]); break; }
    xSemaphoreGive(s_ws_mtx);
}

static bool ws_any_clients() {
    if (!s_ws_mtx) return false;
    bool any = false;
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fds[i] != -1) { any = true; break; }
    xSemaphoreGive(s_ws_mtx);
    return any;
}

// Each async send owns a heap copy of the payload; httpd frees it via ws_transfer_complete when the
// transfer finishes. httpd_ws_send_data_async does not block the caller, so the broadcast task can
// fan out to every client and move on.
struct WsPacketContext {
    std::string      payload;
    httpd_ws_frame_t frame;
};

// Runs on the httpd task once the frame has been written (or has failed to write). Frees the copy
// and settles this client's backpressure state; a client that keeps failing is closed here, which
// is the only thing that ends the 5 s send stall it imposes on the httpd task every tick.
static void ws_transfer_complete(esp_err_t err, int fd, void* arg) {
    delete static_cast<WsPacketContext*>(arg);
    if (ws_release_inflight(fd, err == ESP_OK)) {
        ESP_LOGW(TAG, "ws: subscriber fd=%d failed %u sends in a row — closing it",
                 fd, static_cast<unsigned>(tk::WS_MAX_FAILS));
        // close_fn (http_server.cpp) → http_events_on_close() frees the slot. This only ENQUEUES
        // the close, so it is safe to call from the httpd task we are running on — and it can also
        // be dropped when that same control queue is full, which is likeliest under exactly the
        // congestion that condemned this client. Not a hole: ws_note_completed() leaves the failure
        // streak latched, so the next failed completion re-triggers the close. Log the miss so a
        // repeatedly-deferred eviction is visible in /diag rather than silent.
        if (httpd_sess_trigger_close(s_server, fd) != ESP_OK)
            ESP_LOGW(TAG, "ws: close of fd=%d could not be queued — retrying next failure", fd);
    }
}

// Queue an async copy of [data,len) to every registered client. Runs in the broadcast task, which
// has NO OOM guard above it — so each per-client copy is a nothrow-new + a guarded assign, and any
// client we cannot allocate for this tick is simply skipped (never an abort).
//
// Snapshot the fd list under the mutex, then send with the lock RELEASED: the per-client heap
// copies and the async-queue calls no longer run under s_ws_mtx, so a concurrent "sub"/close
// (ws_register_client / http_events_on_close) never waits on a whole fan-out. A fd that closes in
// the send window just makes httpd_ws_send_data_async fail → we free that ctx; the fd can't be
// reused underneath us because close_fn runs on the httpd task, which is also where the async send
// is dispatched, so the send is serialized after any close it observes.
static void ws_send_to_all(const char* data, size_t len) {
    if (!s_server || !s_ws_mtx) return;
    int fds[WS_MAX_CLIENTS];
    int n = 0;
    xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (s_ws_fds[i] != -1) fds[n++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mtx);

    for (int i = 0; i < n; i++) {
        // Backpressure FIRST, before any allocation: a client still sitting on WS_MAX_INFLIGHT
        // uncompleted frames is not draining, so it gets nothing this tick. Without this the
        // broadcast task out-produced a stalled client 2.5:1 and the backlog ate the heap.
        if (!ws_reserve_inflight(fds[i])) continue;
        WsPacketContext* ctx = new (std::nothrow) WsPacketContext();
        if (!ctx) { ws_cancel_inflight(fds[i]); continue; }   // our OOM — don't score it as the client's
        try {
            ctx->payload.assign(data, len);
        } catch (...) {
            delete ctx;
            ws_cancel_inflight(fds[i]);
            continue;
        }
        memset(&ctx->frame, 0, sizeof(httpd_ws_frame_t));
        ctx->frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(ctx->payload.c_str()));
        ctx->frame.len     = ctx->payload.size();
        ctx->frame.type    = HTTPD_WS_TYPE_TEXT;
        ctx->frame.final   = true;
        // The in-flight accounting rests on this contract: a non-ESP_OK return is the ONLY case
        // where ws_transfer_complete will not run. That holds from ESP-IDF v5.5.5 (the pinned
        // version — .github/workflows/build.yml), whose httpd_queue_work reserves a control-queue
        // slot up front and fails fast when the queue is full. Older IDFs (v5.4.x, v5.5) let a
        // full lwIP UDP mbox drop the control datagram while sendto still reports success, so
        // ESP_OK could be returned for work that never runs — leaking this ctx AND pinning
        // in_flight at the cap forever, silencing the subscriber with no failed completion to
        // evict it on. If the IDF pin is ever moved BACK, this needs a timeout-based reaper.
        if (httpd_ws_send_data_async(s_server, fds[i], &ctx->frame,
                                     ws_transfer_complete, ctx) != ESP_OK) {
            delete ctx;   // stale fd (client vanished before the send) — clean up
            // Counts against the client: an fd we cannot even queue for is on its way out, and the
            // completion callback that would normally settle this will never run.
            ws_release_inflight(fds[i], false);
        }
    }
}

// Build the /status JSON and push it to every subscriber. build_status_object() reads std::string
// getters (VIN, broker, syslog host) that can throw std::bad_alloc, and this runs in the broadcast
// task with nothing to catch above it, so the whole build is guarded: an OOM skips this tick and
// keeps the task (and the device) alive.
static void ws_broadcast_status() {
    if (!s_server || !s_ws_mtx || !ws_any_clients()) return;
    char* json = nullptr;
    try {
        cJSON* root = build_status_object();
        if (!root) return;
        json = cJSON_PrintUnformatted(root);   // NULL (not a throw) on a fragmented heap
        cJSON_Delete(root);
    } catch (...) {
        return;
    }
    if (!json) return;
    ws_send_to_all(json, strlen(json));
    free(json);
}

static void ws_broadcast_task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WS_BROADCAST_INTERVAL_MS));
        ws_broadcast_status();
    }
}

// The /events protocol is one command: a "sub" text frame earns an immediate /status snapshot and a
// slot in the broadcast list, after which the broadcast task pushes it frames. Returning ESP_FAIL
// makes esp_http_server close and clean up the socket — the intended response to a frame we could
// neither read nor skip past, whose unread body would otherwise be parsed as the next frame's
// header. close_fn (http_server.cpp) unregisters the fd on the way out.
static esp_err_t h_ws_events(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        return ESP_OK;   // WebSocket handshake — no frame to read yet
    }

    // max_len = 0 reads the header only, filling in the frame's type and its ANNOUNCED length.
    httpd_ws_frame_t ws_pkt = {};
    if (httpd_ws_recv_frame(req, &ws_pkt, 0) != ESP_OK) return ESP_FAIL;

    switch (tk::ws_frame_plan(ws_pkt.len)) {
        case tk::WsPlan::Skip:
            return ESP_OK;
        case tk::WsPlan::Reject:
            ESP_LOGW(TAG, "ws: frame of %llu B exceeds the %u B command buffer — closing",
                     static_cast<unsigned long long>(ws_pkt.len),
                     static_cast<unsigned>(tk::WS_CMD_MAX));
            return ESP_FAIL;
        case tk::WsPlan::Read:
            break;
    }

    uint8_t buf[tk::WS_CMD_MAX] = {};
    ws_pkt.payload = buf;
    if (httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf)) != ESP_OK) return ESP_FAIL;

    if (tk::ws_frame_action(ws_pkt.type == HTTPD_WS_TYPE_TEXT,
                            reinterpret_cast<const char*>(buf), ws_pkt.len) != tk::WsAction::Subscribe) {
        return ESP_OK;   // not a command we know — no snapshot, and no broadcast slot
    }

    // Subscribe only now: registering on any frame at all meant a client that never asked was still
    // pushed a frame every cycle, and kept a slot from one that had.
    if (!ws_register_client(httpd_req_to_sockfd(req))) {
        ESP_LOGW(TAG, "ws: broadcast list full — /events subscriber not registered");
        return ESP_OK;
    }

    // Immediate first-paint snapshot, sent synchronously on this httpd session task. Registered raw
    // (not under handle_all), so guard the build here too — an OOM drops the snapshot (the periodic
    // push catches up) instead of unwinding through httpd's C dispatch → std::terminate → reboot.
    try {
        cJSON* root = build_status_object();
        if (root) {
            char* json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (json) {
                httpd_ws_frame_t f = {};
                f.payload = reinterpret_cast<uint8_t*>(json);
                f.len     = strlen(json);
                f.type    = HTTPD_WS_TYPE_TEXT;
                f.final   = true;
                httpd_ws_send_frame(req, &f);
                free(json);
            }
        }
    } catch (...) {
        return ESP_OK;
    }
    return ESP_OK;
}

void http_events_register(httpd_handle_t server) {
    s_server = server;
    if (!s_ws_mtx) s_ws_mtx = xSemaphoreCreateMutex();

    // Registered BEFORE the /* wildcard handlers (see http_server_start) so a GET /events matches
    // this WS route, not the catch-all — esp_http_server checks handlers in registration order.
    httpd_uri_t ws = {};
    ws.uri          = "/events";
    ws.method       = HTTP_GET;
    ws.handler      = h_ws_events;
    ws.user_ctx     = nullptr;
    ws.is_websocket = true;
    httpd_register_uri_handler(server, &ws);

    // Background pusher. Priority below httpd's (5) — it's a best-effort UI feed, never ahead of
    // serving requests. Stack: build_status_object() + cJSON_PrintUnformatted are heap-backed and
    // light on stack, but this is the SAME build the httpd task runs on 8192, and an overflow in
    // this net-less task would abort() (defeating car sleep) — so 6144 keeps a real margin over the
    // measured-elsewhere path rather than halving it, without paying the full 8192 on a tight heap.
    xTaskCreate(ws_broadcast_task, "ws_bcast", 6144, nullptr, tk::kPrioWsBroadcast, nullptr);
    ESP_LOGI(TAG, "/events WebSocket registered; pushing /status every %u ms",
             static_cast<unsigned>(WS_BROADCAST_INTERVAL_MS));
}
