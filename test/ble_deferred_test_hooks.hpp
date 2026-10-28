#pragma once

// Passive host effects only. These counters do not implement queue, framing, admission or crypto.
// The real production methods and pure FIFO are compiled into the existing native Client facade.
static unsigned native_mailbox_depth = 0;
static unsigned native_mailbox_lock_violations = 0;
static unsigned native_external_lock_violations = 0;

struct NativeMailboxMux { unsigned held{0}; };
using portMUX_TYPE = NativeMailboxMux;
#define portMUX_INITIALIZER_UNLOCKED {}

static void native_require_unlocked() noexcept {
    if (native_mailbox_depth != 0) ++native_external_lock_violations;
}
static void native_mailbox_enter(NativeMailboxMux* mux) noexcept {
    if (!mux || mux->held != 0) ++native_mailbox_lock_violations;
    if (mux) ++mux->held;
    ++native_mailbox_depth;
}
static void native_mailbox_exit(NativeMailboxMux* mux) noexcept {
    if (!mux || mux->held == 0 || native_mailbox_depth == 0) {
        ++native_mailbox_lock_violations;
        return;
    }
    --mux->held;
    --native_mailbox_depth;
}

struct NativeLifecycleTrace {
    std::array<char, 64> bytes{};
    size_t size{0};
    bool overflow{false};
    void push_back(char value) noexcept {
        if (size == bytes.size()) { overflow = true; return; }
        bytes[size++] = value;
    }
    bool equals(std::initializer_list<char> expected) const noexcept {
        return !overflow && size == expected.size() &&
               std::equal(expected.begin(), expected.end(), bytes.begin());
    }
};
