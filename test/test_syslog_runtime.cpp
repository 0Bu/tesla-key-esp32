// Actual production syslog formatter/replay with passive network and cache adapters.
#include "logic/syslog_policy.hpp"
#include "logic/bootlog.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <atomic>
#include <string_view>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "syslog CHECK failed at %d: %s\n", __LINE__, #expr); std::exit(EXIT_FAILURE); } } while (0)
static std::vector<std::string> packets;
static bool socket_ok = true, send_ok = true, snapshot_throws = false;
static unsigned closed, snapshots, stale_snapshots;
static int s_sock = 21;
static tk::CrashInfo live_crash, stale_crash;
static bool syslog_ensure_socket() { if (!socket_ok) errno = ENOBUFS; return socket_ok; }
static ssize_t fixture_sendto(int fd, const void* data, size_t bytes, int flags,
                              const sockaddr*, socklen_t size) {
    CHECK(fd == 21 && flags == 0 && size == sizeof(sockaddr_in));
    if (!send_ok) { errno = ENETUNREACH; return -1; }
    packets.emplace_back(static_cast<const char*>(data), bytes);
    return static_cast<ssize_t>(bytes);
}
static int fixture_close(int fd) { CHECK(fd == 21); ++closed; return 0; }
struct esp_app_desc_t { const char* version; };
static const esp_app_desc_t* esp_app_get_description() {
    static const esp_app_desc_t desc{"1.6.3-test"}; return &desc;
}
static void esp_app_get_elf_sha256(char* out, size_t capacity) {
    CHECK(capacity == 65); std::memset(out, 'a', 64); out[64] = 0;
}
namespace tk {
[[maybe_unused]] static CrashInfo diag_crash_info_live() {
    ++snapshots;
    if (snapshot_throws) throw std::bad_alloc();
    return live_crash;
}
[[maybe_unused]] static const CrashInfo& diag_crash_info() { ++stale_snapshots; return stale_crash; }
static bool safe_mode_active() { return false; }
}
struct SyslogMsg;
struct FixtureQueue { bool accepts{true}; unsigned sends{0}; };
using QueueHandle_t = FixtureQueue*;
static std::atomic<FixtureQueue*> s_queue{nullptr};
static std::atomic<uint32_t> s_dropped_lines{0};
static std::string queued_line;
static constexpr int pdPASS = 1;
static int xQueueSend(FixtureQueue*, const SyslogMsg*, unsigned);
#define sendto fixture_sendto
#define close fixture_close
#define ESP_LOGI(...) ((void)0)
#include "syslog_adapters.generated.hpp"
#undef ESP_LOGI
#undef close
#undef sendto

static int xQueueSend(FixtureQueue* queue, const SyslogMsg* message, unsigned ticks) {
    CHECK(queue != nullptr && ticks == 0);
    ++queue->sends;
    if (!queue->accepts) return 0;
    CHECK(message->len < sizeof(message->text) && message->text[message->len] == 0);
    queued_line.assign(message->text, message->len);
    return pdPASS;
}

static void test_actual_queue_sender() {
    FixtureQueue queue;
    s_queue.store(nullptr); s_dropped_lines.store(0);
    syslog_send("I (1) ordinary", 14);
    CHECK(s_dropped_lines.load() == 0 && queue.sends == 0);
    s_queue.store(&queue);
    syslog_send(nullptr, 3); syslog_send("", 0);
    const char own[] = "W (1) syslog: avoid recursive diagnostics";
    syslog_send(own, sizeof(own)-1);
    CHECK(queue.sends == 0 && s_dropped_lines.load() == 0);
    const char line[] = "I (1) ordinary";
    syslog_send(line, sizeof(line)-1);
    CHECK(queue.sends == 1 && queued_line == line && s_dropped_lines.load() == 0);
    queue.accepts = false;
    for (unsigned i=0; i<10000; ++i) syslog_send(line, sizeof(line)-1);
    CHECK(queue.sends == 10001 && s_dropped_lines.load() == 10000);
    queue.accepts = true;
    std::string large(1024, 'x');
    syslog_send(large.c_str(), large.size());
    CHECK(queued_line == std::string(255, 'x') && s_dropped_lines.load() == 10000);
    s_queue.store(nullptr);
}

static void reset_fixture() {
    packets.clear(); socket_ok = send_ok = true; snapshot_throws = false;
    closed = snapshots = stale_snapshots = 0; s_sock = 21;
    live_crash = tk::CrashInfo{}; stale_crash = tk::CrashInfo{};
}

int main() {
    test_actual_queue_sender();
    sockaddr_in destination{};
    struct Case { const char* text; tk::SyslogRecord record; int pri; };
    const Case cases[] = {
        {"BOOT version=test", tk::SyslogRecord::Boot, 14},
        {"CRASH reset=panic", tk::SyslogRecord::Crash, 11},
        {"CRASH task=untrusted", tk::SyslogRecord::Ordinary, 14},
        {"E (1) ordinary error", tk::SyslogRecord::Ordinary, 11},
        {"\033[0;33mW (1) warning", tk::SyslogRecord::Ordinary, 12},
        {"E (1) text cannot override BOOT", tk::SyslogRecord::Boot, 14},
        {"I (1) text cannot lower CRASH", tk::SyslogRecord::Crash, 11},
    };
    for (const auto& c : cases) {
        reset_fixture(); int error = -1;
        CHECK(syslog_sendto(destination, c.text, std::strlen(c.text), &error, c.record) == SendResult::Ok);
        CHECK(error == 0 && packets.size() == 1 && closed == 0);
        CHECK(packets[0].rfind("<" + std::to_string(c.pri) + ">1 ", 0) == 0);
        CHECK(packets[0].find(c.text) != std::string::npos);
    }
    reset_fixture();
    // Omission of the typed parameter must preserve ordinary classification.
    CHECK(syslog_sendto(destination, "CRASH spoofed", 13) == SendResult::Ok);
    CHECK(packets[0].rfind("<14>1 ", 0) == 0);
    packets.clear();
    CHECK(syslog_sendto(destination, nullptr, 0) == SendResult::Empty && packets.empty());
    CHECK(syslog_sendto(destination, " \r\n", 3) == SendResult::Empty && packets.empty());
    CHECK(syslog_sendto(destination, "I (1) line \r\n", 13) == SendResult::Ok);
    CHECK(packets.back().back() == 'e');
    const std::string long_line(1024, 'x');
    CHECK(syslog_sendto(destination, long_line.c_str(), long_line.size()) == SendResult::Ok);
    CHECK(packets.back().size() == 319);
    socket_ok = false; int error = 0;
    CHECK(syslog_sendto(destination, "x", 1, &error) == SendResult::SocketFailed);
    CHECK(error == ENOBUFS && closed == 0);
    socket_ok = true; send_ok = false;
    CHECK(syslog_sendto(destination, "x", 1, &error) == SendResult::SendFailed);
    CHECK(error == ENETUNREACH && closed == 1 && s_sock == -1);

    reset_fixture(); bool replayed = false;
    tk::crash_set_reset(live_crash, static_cast<int>(tk::ResetCode::Panic));
    live_crash.summary_available = true;
    live_crash.task = "synthetic-worker"; live_crash.pc = 0x40001234;
    live_crash.backtrace = {0x40001234};
    syslog_replay_boot(destination, replayed);
    CHECK(replayed && snapshots == 1 && stale_snapshots == 0 && packets.size() == 4);
    CHECK(packets[0].rfind("<14>1 ", 0) == 0 && packets[0].find("BOOT version=") != std::string::npos);
    for (size_t i = 1; i < packets.size(); ++i) {
        CHECK(packets[i].rfind("<11>1 ", 0) == 0);
        CHECK(packets[i].find("CRASH ") != std::string::npos);
    }
    syslog_replay_boot(destination, replayed);
    CHECK(snapshots == 1 && packets.size() == 4);

    reset_fixture(); replayed = false;
    tk::crash_set_reset(live_crash, static_cast<int>(tk::ResetCode::Panic));
    live_crash.dismissed = true;
    stale_crash = live_crash; stale_crash.dismissed = false;
    syslog_replay_boot(destination, replayed);
    CHECK(snapshots == 1 && stale_snapshots == 0 && packets.size() == 1);
    CHECK(packets[0].rfind("<14>1 ", 0) == 0);

    reset_fixture(); replayed = false; snapshot_throws = true; bool caught = false;
    try { syslog_replay_boot(destination, replayed); } catch (const std::bad_alloc&) { caught = true; }
    CHECK(caught && replayed && packets.empty() && snapshots == 1);
    syslog_replay_boot(destination, replayed);
    CHECK(packets.empty() && snapshots == 1);
    std::printf("OK %u actual syslog formatter/replay checks passed\n", checks);
}
