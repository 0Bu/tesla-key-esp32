#pragma once
// /events WebSocket command policy (http_events.cpp). Pure, IDF-free, host-tested (test/test_logic.cpp).
//
// /events speaks exactly one command — the client sends the text "sub" and gets a live /status
// snapshot, after which a broadcast task pushes it the /status JSON on a fixed cadence. Everything
// here decides what an arriving frame MEANS, split in two because the handler must decide twice:
// first from the announced length (before any payload is touched), then from the bytes it read.
//
// Why the length is a PLAN input and never an allocation. httpd_ws_recv_frame(max_len=0) parses
// only the header, so `len` at that point is a number the client asserted — RFC 6455 allows 64
// bits of it, and nothing has been read to back it up. On a chip whose binding limit is the
// largest contiguous free block, sizing a buffer from it would hand any client on the LAN a
// one-frame OOM. So the frame is measured against a fixed command buffer and refused if it does
// not fit; it is never accommodated.
//
// Why refusing has to mean CLOSING. esp_http_server's recv fails a too-long frame outright
// (ESP_ERR_INVALID_SIZE) and, crucially, leaves the body in the socket — and it offers no way to
// skip N bytes. So after an oversized frame the stream is desynchronized: the unread payload would
// be parsed as the next frame's header. Draining it is exactly the allocation we just refused to
// make, which leaves dropping the connection as the only honest exit. Our own client only ever
// sends 3 bytes, so this costs a real user nothing.
#include <cstddef>
#include <cstdint>

namespace tk {

// The fixed command buffer /events reads a frame into. "sub" is the whole vocabulary; the headroom
// is only so a slightly-off client (a trailing newline, say) still parses rather than being dropped.
inline constexpr size_t WS_CMD_MAX = 16;

// What to do with a frame of a given announced length, decided BEFORE its payload is touched.
enum class WsPlan : uint8_t {
    Skip,     // empty frame: carries no command, and has no body to leave behind — just ignore it
    Read,     // fits the command buffer: safe to read, then classify with ws_frame_action()
    Reject,   // longer than any command we accept: undrainable, so the connection must go
};

inline constexpr WsPlan ws_frame_plan(size_t announced_len) {
    if (announced_len == 0)         return WsPlan::Skip;
    if (announced_len > WS_CMD_MAX) return WsPlan::Reject;
    return WsPlan::Read;
}

// ─── Per-subscriber send backpressure ────────────────────────────────────────────────────────
//
// Why this exists (the 2026-07-18 wedge). A subscriber that stops READING — a laptop suspended
// with the UI open, a backgrounded tab — leaves its TCP send buffer full. esp_http_server's send
// then blocks the httpd task for the whole send_wait_timeout (HTTPD_DEFAULT_CONFIG: 5 s) before
// failing with EAGAIN. The broadcast task pushes every 2 s, so it queued frames 2.5x faster than
// that one client could retire them, and each queued frame owns a heap copy of the whole /status
// JSON. The backlog grew without bound: free heap fell 69 KB → 8 KB in ~7 minutes, the largest
// contiguous block 31744 B → 544 B, after which every allocation threw. The device did not crash
// and did not reboot — it wedged, spinning bad_alloc in the vehicle loop for ten hours, unreachable.
//
// Two bounds make that impossible, and neither depends on noticing the stall in time:
//   • in_flight is capped, so the backlog has a ceiling no matter how slow the client is;
//   • a client that fails WS_MAX_FAILS completions in a row is closed outright, which also ends
//     the 5 s httpd stall it was costing every tick. Closing is cheap for a real user: app.js
//     reconnects 3 s later, so a browser that merely slept resubscribes on wake.
//
// Kept as pure state + transitions (rather than inline bookkeeping) so the cases that matter —
// the cap holding under a client that never completes, the failure streak resetting on one good
// send — are exercised on the host instead of only on a wedged device.

// At most this many frames queued-but-not-yet-completed per client. 2 lets a healthy client stay
// pipelined across a tick boundary while bounding the backlog to two payload copies.
inline constexpr uint8_t WS_MAX_INFLIGHT = 2;

// Consecutive failed completions before the subscriber is closed. 3 rides out a transient blip
// (~15 s at the 5 s send timeout) without tolerating a client that is simply gone.
inline constexpr uint8_t WS_MAX_FAILS = 3;

struct WsClientSend {
    uint8_t in_flight = 0;   // queued to httpd, completion callback not yet run
    uint8_t fails     = 0;   // consecutive failed completions
};

// May another frame be queued to this client this tick? False means "still draining" — skip it
// rather than piling on, which is precisely the bound the incident was missing.
inline constexpr bool ws_may_send(const WsClientSend& c) { return c.in_flight < WS_MAX_INFLIGHT; }

// A frame was handed to httpd for this client. Saturates rather than wrapping: an 8-bit counter
// that wrapped to 0 would silently re-open the floodgate.
inline void ws_note_queued(WsClientSend& c) {
    if (c.in_flight < UINT8_MAX) c.in_flight++;
}

// A send completed (or failed). Returns true when this client has now failed WS_MAX_FAILS in a
// row and should be closed. One successful send clears the streak.
inline bool ws_note_completed(WsClientSend& c, bool sent_ok) {
    if (c.in_flight) c.in_flight--;
    if (sent_ok) { c.fails = 0; return false; }
    if (c.fails < UINT8_MAX) c.fails++;
    return c.fails >= WS_MAX_FAILS;
}

// What a frame we successfully read actually asks for.
enum class WsAction : uint8_t {
    Ignore,      // not a command we know — no snapshot, and no slot in the broadcast list
    Subscribe,   // a "sub" text frame: send the snapshot and start pushing to this socket
};

// `payload`/`len` are the bytes ACTUALLY received — call this only after a read that returned OK,
// never on a buffer a failed read left untouched. A prefix match (rather than an exact 3 bytes) is
// deliberate: a trailing newline or "subscribe" from a hand-rolled client still subscribes.
inline WsAction ws_frame_action(bool is_text, const char* payload, size_t len) {
    if (!is_text || !payload || len < 3) return WsAction::Ignore;
    if (payload[0] == 's' && payload[1] == 'u' && payload[2] == 'b') return WsAction::Subscribe;
    return WsAction::Ignore;
}

} // namespace tk
