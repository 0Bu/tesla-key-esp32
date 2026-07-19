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
