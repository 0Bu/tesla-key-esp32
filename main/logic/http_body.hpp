#pragma once
// Request-body reassembly (http_common.cpp read_body, provisioning.cpp save_post). Pure, IDF-free,
// host-tested (test/test_logic.cpp).
//
// A POST body is a TCP stream, not a datagram: esp_http_server hands over whatever has arrived, and
// its own docs say so — "If content_len is too large for the buffer then user may have to make
// multiple calls to this function". Both call sites called httpd_req_recv exactly once and treated
// the result as the whole body, so a body split across segments arrived truncated: the JSON handler
// answered 400 to a valid POST (worst case: a charging command silently ran with amps=0 yet returned
// success), and provisioning could persist a truncated credential. Rare on a quiet LAN, reliable on
// a busy/distant one — the worst shape of bug to own.
//
// Why the loop lives here rather than in the .cpp. The reassembly IS the fix, and on the device it is
// only exercised by a segmentation pattern we cannot ask for. Behind a recv callback it becomes a
// handful of CHECKs — one-byte-at-a-time delivery, a mid-body error, a stalled peer — that run in CI
// on every push. The .cpp keeps the one thing that is genuinely IDF's: mapping httpd_req_recv's
// return codes onto BodyRecv.
//
// Why a stalled peer must lose. httpd_req_recv returns HTTPD_SOCK_ERR_TIMEOUT when its socket timeout
// expires with nothing new — recoverable, worth retrying, but only a BOUNDED number of times.
// Retrying forever would let one client that opens a POST, announces a Content-Length and then goes
// quiet park the single httpd task indefinitely, taking the whole web UI (and the OTA route out of a
// bad config) down with it. A body that cannot finish within a couple of socket timeouts is not one
// worth waiting for.
#include <cstddef>
#include <cstdint>

namespace tk {

// One recv attempt, classified. esp_http_server returns bytes>0, 0 for a peer that closed,
// HTTPD_SOCK_ERR_TIMEOUT (-3) for "nothing arrived in time", and other negatives for hard errors.
// Keeping that mapping in the caller is what keeps this header IDF-free.
enum class BodyRecv : uint8_t {
    Data,      // `bytes` bytes were written into the buffer
    Timeout,   // nothing arrived within the socket timeout — recoverable, bounded by BODY_MAX_IDLE
    Error,     // peer closed, or an unrecoverable socket error
};

struct BodyChunk {
    BodyRecv kind;
    size_t   bytes;   // meaningful only when kind == Data
};

// Consecutive timeouts tolerated before a body is abandoned. Each is a full socket timeout
// (CONFIG_HTTPD_REQ_RECV_TMO, 5 s by default), so ~15 s of patience for a body a healthy client
// sends in one segment.
inline constexpr int BODY_MAX_IDLE = 2;

// Read exactly `total` bytes into `buf` and NUL-terminate. Returns the byte count, or -1 if the body
// does not fit `cap` (leaving room for the terminator), is empty, or the peer failed to deliver it.
// `recv(dst, len)` must return a BodyChunk.
template <typename Recv>
int http_body_read(char* buf, size_t cap, size_t total, Recv recv) {
    if (!buf || total == 0 || total >= cap) return -1;

    size_t got  = 0;
    int    idle = 0;
    while (got < total) {
        const BodyChunk c = recv(buf + got, total - got);
        if (c.kind == BodyRecv::Timeout) {
            if (++idle > BODY_MAX_IDLE) return -1;
            continue;
        }
        // A recv reporting more than we asked for would run off the buffer. It cannot happen against
        // a correct socket layer, but `bytes` decides a write bound, so it is checked, not trusted.
        if (c.kind == BodyRecv::Error || c.bytes == 0 || c.bytes > total - got) return -1;
        idle = 0;
        got += c.bytes;
    }
    buf[got] = '\0';
    return static_cast<int>(got);
}

} // namespace tk
