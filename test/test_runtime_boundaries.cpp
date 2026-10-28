#include "diag_log.hpp"
#include "nvs_storage.hpp"
#include "mqtt_probe_owner.hpp"
#include "ping_probe.hpp"
#include "reboot_reason.hpp"
#include "safe_mode.hpp"
#include "ota_update.hpp"
#include "runtime_fault_log.hpp"
#include "vehicle_ctrl_internal.hpp"
#include "stack_watch.hpp"
#include "logic/redact.hpp"
#include "logic/nimble_start_gate.hpp"
#include "logic/runtime_admission.hpp"
#include "logic/syslog_start_gate.hpp"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

int test_coredump_hash();
#ifdef TEST_OTA_ADMISSION_ADAPTER
int test_ota_admission();
#endif
#ifdef TEST_BACKGROUND_POLL_ADAPTER
int test_background_poll();
#endif

struct HostPingSession {
    esp_ping_callbacks_t callbacks{};
    std::uint32_t replies{0};
};

namespace {

int checks = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        ++checks;                                                                \
        if (!(condition)) {                                                      \
            std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ": "         \
                      << #condition << '\n';                                     \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

vprintf_like_t log_sink = nullptr;
bool capture_log_sinks = false;
std::string captured_serial;
std::string captured_syslog;
HostSemaphore* last_mutex = nullptr;
bool fail_diag_mutex_create = false;
unsigned log_hook_installs = 0;
TickType_t last_mutex_wait = portMAX_DELAY;
bool inject_hook_reentry = false;
bool inject_mutex_reentry = false;
unsigned hook_reentries = 0;
unsigned mutex_reentries = 0;
std::string captured_fallback;
TaskFunction_t created_task = nullptr;
void* created_task_arg = nullptr;
BaseType_t task_create_result = pdPASS;
int deleted_tasks = 0;
TickType_t last_delay = 0;
TickType_t fake_ticks = 0;
UBaseType_t fake_stack_free = 0;
unsigned stack_queries = 0;
bool fake_isr = false;
bool fake_can_yield = true;
BaseType_t fake_scheduler_state = taskSCHEDULER_RUNNING;
int current_task_token = 0;
TaskHandle_t fake_current_task = &current_task_token;
bool track_log_stack_queries = false;
char log_stack_trace[16]{};
unsigned log_stack_trace_size = 0;
int throw_serial_sink = 0;
int throw_syslog_sink = 0;

void log_stack_step(char step) {
    if (!track_log_stack_queries) return;
    CHECK(log_stack_trace_size + 1 < sizeof(log_stack_trace));
    log_stack_trace[log_stack_trace_size++] = step;
    log_stack_trace[log_stack_trace_size] = '\0';
}

enum class PingScenario {
    SetupFail,
    StartFail,
    StartDeleteFail,
    Reply,
    NoReply,
    ProfileFail,
    TimeoutLateEnd,
};

PingScenario ping_scenario = PingScenario::Reply;
int ping_new_calls = 0;
int ping_start_calls = 0;
int ping_stop_calls = 0;
int ping_delete_calls = 0;
int ping_delete_failures_remaining = 0;
HostPingSession* last_ping_session = nullptr;
tk::PingProbeCallbackArgs captured_ping_args{};

bool load_ok = true;
bool save_ok = true;
bool remove_ok = true;
bool throw_on_load = false;
bool throw_on_save = false;
bool throw_on_remove = false;
bool throw_other_on_load = false;
bool throw_other_on_save = false;
bool fail_global_allocation = false;
std::string stored_count;
bool stored_present = false;
std::vector<std::string> saved_counts;
unsigned remove_calls = 0;

struct FakeMqttProbeOps {
    using Client = void*;
    using Semaphore = void*;
    static std::vector<std::string> events;
    static void stop(Client) noexcept { events.emplace_back("stop"); }
    static void destroy(Client) noexcept { events.emplace_back("destroy"); }
    static void delete_semaphore(Semaphore) noexcept { events.emplace_back("delete_sem"); }
};

std::vector<std::string> FakeMqttProbeOps::events;

int discard_log(const char* fmt, va_list ap) {
    if (throw_serial_sink == 1) throw std::bad_alloc();
    if (throw_serial_sink == 2) throw 17;
    if (capture_log_sinks) {
        char buffer[1024];
        const int n = vsnprintf(buffer, sizeof(buffer), fmt, ap);
        CHECK(n >= 0 && static_cast<size_t>(n) < sizeof(buffer));
        captured_serial.assign(buffer, n);
    }
    return 0;
}

int emit_log(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    try {
        const int result = log_sink ? log_sink(format, ap) : 0;
        va_end(ap);
        return result;
    } catch (...) {
        va_end(ap);
        throw;
    }
}
void reset_storage(const char* value) {
    load_ok = true;
    save_ok = true;
    remove_ok = true;
    throw_on_load = false;
    throw_on_save = false;
    throw_on_remove = false;
    throw_other_on_load = false;
    throw_other_on_save = false;
    stored_count = value;
    stored_present = value && *value != '\0';
    saved_counts.clear();
    remove_calls = 0;
}

void reset_ping_capture(PingScenario scenario) {
    ping_scenario = scenario;
    ping_new_calls = 0;
    ping_start_calls = 0;
    ping_stop_calls = 0;
    ping_delete_calls = 0;
    ping_delete_failures_remaining = 0;
    last_ping_session = nullptr;
    captured_ping_args = {};
    fake_ticks = 0;
}

}  // namespace

void* operator new(std::size_t size) {
    if (fail_global_allocation) throw std::bad_alloc();
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    if (fail_global_allocation) throw std::bad_alloc();
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

struct HostSemaphore {
    bool held{false};
    bool binary{false};
    bool signaled{false};
    unsigned takes{0};
    unsigned gives{0};
};

extern "C" SemaphoreHandle_t xSemaphoreCreateMutex() {
    if (inject_mutex_reentry) {
        ++mutex_reentries;
        CHECK(diag_log_dump_chunks([](const char*, size_t) { return true; }) == DiagDumpResult::SnapshotInvalidated);
        emit_log("E (4) mqtt_client: Error parse uri (1) = mqtt://example:secret@broker.invalid\n");
    }
    if (fail_diag_mutex_create) { last_mutex = nullptr; return nullptr; }
    last_mutex = new HostSemaphore();
    return last_mutex;
}

extern "C" SemaphoreHandle_t xSemaphoreCreateBinary() {
    auto* semaphore = new HostSemaphore();
    semaphore->binary = true;
    return semaphore;
}

extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks) {
    if (!sem) return pdFALSE;
    if (sem->binary) {
        ++sem->takes;
        if (sem->signaled) {
            sem->signaled = false;
            return pdTRUE;
        }
        fake_ticks += ticks;
        return pdFALSE;
    }
    last_mutex_wait = ticks;
    if (!sem || sem->held) return pdFALSE;
    sem->held = true;
    ++sem->takes;
    return pdTRUE;
}

extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    if (!sem) return pdFALSE;
    if (sem->binary) {
        sem->signaled = true;
        ++sem->gives;
        return pdTRUE;
    }
    if (!sem->held) return pdFALSE;
    sem->held = false;
    ++sem->gives;
    return pdTRUE;
}

extern "C" void vSemaphoreDelete(SemaphoreHandle_t sem) { delete sem; }

extern "C" BaseType_t xPortInIsrContext() {
    log_stack_step('I'); return fake_isr ? pdTRUE : pdFALSE;
}
extern "C" bool xPortCanYield() {
    log_stack_step('Y'); return fake_can_yield;
}
extern "C" BaseType_t xTaskGetSchedulerState() {
    log_stack_step('S'); return fake_scheduler_state;
}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle() {
    log_stack_step('C'); return fake_current_task;
}
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    log_stack_step('H');
    if (track_log_stack_queries) CHECK(!last_mutex || !last_mutex->held);
    CHECK(task==nullptr); ++stack_queries; return fake_stack_free;
}

static void test_actual_guard_unwind_and_stack_minima() {
    // The real RAII types release both ordinary and non-standard exception paths.
    for (int fault : {0,1,2}) {
        HostSemaphore sem{};
        std::atomic<bool> in_flight{false};
        try {
            tk::SemGuard guard(&sem,0); CHECK(guard.acquired()); CHECK(sem.held);
            tk::InFlightGuard flight(in_flight); CHECK(in_flight.load());
            if (fault==1) throw std::bad_alloc();
            if (fault==2) throw 17;
        } catch (...) {}
        CHECK(!in_flight.load()); CHECK(!sem.held); CHECK(sem.takes==1 && sem.gives==1);
    }
    HostSemaphore occupied{}; occupied.held=true;
    { tk::SemGuard guard(&occupied,0); CHECK(!guard); }
    CHECK(occupied.held && occupied.gives==0);
    { tk::SemGuard guard(nullptr,0); CHECK(!guard); }

    // Compile stack_watch.cpp itself. A measured lower bound0 remains sampled; rising observations do
    // not increase the retained minimum, and an invalid index never queries another task.
    const auto slot=tk::StackWatch::Httpd;
    CHECK(!tk::stack_watch_min_free_bytes(slot).has_value());
    CHECK(!tk::stack_watch_min_free_bytes(tk::StackWatch::Mqtt).has_value());
    for (uint32_t value : {1000U,1200U,600U,900U,0U,2000U}) {
        fake_stack_free=value; tk::stack_watch_sample(slot);
        const uint32_t expected=value==1000U||value==1200U?768U:value==600U||value==900U?512U:0U;
        CHECK(tk::stack_watch_min_free_bytes(slot).has_value());
        CHECK(*tk::stack_watch_min_free_bytes(slot)==expected);
    }
    fake_stack_free=456; tk::stack_watch_sample(tk::StackWatch::Vehicle);
    CHECK(*tk::stack_watch_min_free_bytes(tk::StackWatch::Vehicle)==256);
    CHECK(*tk::stack_watch_min_free_bytes(slot)==0);
    const auto before=stack_queries;
    tk::stack_watch_sample(tk::StackWatch::Count);
    CHECK(!tk::stack_watch_min_free_bytes(tk::StackWatch::Count).has_value());
    CHECK(stack_queries==before);
}

extern "C" BaseType_t xTaskCreate(TaskFunction_t task, const char*, uint32_t, void* arg,
                                    UBaseType_t, TaskHandle_t*) {
    if (task_create_result == pdPASS) {
        created_task = task;
        created_task_arg = arg;
    }
    return task_create_result;
}

extern "C" void vTaskDelay(TickType_t ticks) {
    last_delay = ticks;
    fake_ticks += ticks;
}
extern "C" void vTaskDelete(TaskHandle_t) { ++deleted_tasks; }
extern "C" TickType_t xTaskGetTickCount() { return fake_ticks; }

extern "C" esp_err_t esp_ping_new_session(const esp_ping_config_t*,
                                            const esp_ping_callbacks_t* callbacks,
                                            esp_ping_handle_t* out_session) {
    ++ping_new_calls;
    if (ping_scenario == PingScenario::SetupFail || !callbacks || !out_session) {
        if (out_session) *out_session = nullptr;
        return ESP_FAIL;
    }
    auto* session = new HostPingSession();
    session->callbacks = *callbacks;
    session->replies = ping_scenario == PingScenario::Reply ? 2u : 0u;
    last_ping_session = session;
    if (callbacks->cb_args) {
        captured_ping_args =
            *static_cast<const tk::PingProbeCallbackArgs*>(callbacks->cb_args);
    }
    *out_session = session;
    return ESP_OK;
}

extern "C" esp_err_t esp_ping_start(esp_ping_handle_t session) {
    ++ping_start_calls;
    if (!session || ping_scenario == PingScenario::StartFail ||
        ping_scenario == PingScenario::StartDeleteFail) return ESP_FAIL;
    if ((ping_scenario == PingScenario::Reply || ping_scenario == PingScenario::NoReply ||
         ping_scenario == PingScenario::ProfileFail) &&
        session->callbacks.on_ping_end) {
        session->callbacks.on_ping_end(session, session->callbacks.cb_args);
    }
    return ESP_OK;
}

extern "C" esp_err_t esp_ping_stop(esp_ping_handle_t session) {
    ++ping_stop_calls;
    return session ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t esp_ping_delete_session(esp_ping_handle_t session) {
    if (!session) return ESP_FAIL;
    ++ping_delete_calls;
    if (ping_delete_failures_remaining > 0) {
        --ping_delete_failures_remaining;
        return ESP_FAIL;
    }
    if (last_ping_session == session) last_ping_session = nullptr;
    delete session;
    return ESP_OK;
}

extern "C" esp_err_t esp_ping_get_profile(esp_ping_handle_t session, int profile,
                                           void* value, std::size_t value_size) {
    if (ping_scenario == PingScenario::ProfileFail) return ESP_FAIL;
    if (!session || profile != ESP_PING_PROF_REPLY || !value ||
        value_size != sizeof(session->replies)) {
        return ESP_FAIL;
    }
    std::memcpy(value, &session->replies, sizeof(session->replies));
    return ESP_OK;
}

extern "C" vprintf_like_t esp_log_set_vprintf(vprintf_like_t replacement) {
    ++log_hook_installs;
    vprintf_like_t previous = log_sink;
    CHECK(previous != replacement);  // never publish our hook as its own previous sink
    log_sink = replacement;
    if (inject_hook_reentry) {
        ++hook_reentries;
        emit_log("E (3) mqtt_client: Error parse uri (1) = mqtt://example:secret@broker.invalid\n");
    }
    return previous;
}

// Deterministic fallback Serial sink for the publication window before s_prev is stored.
extern "C" int host_test_vprintf(const char* fmt, va_list ap) {
    char buffer[1024];
    const int n = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    CHECK(n >= 0 && static_cast<size_t>(n) < sizeof(buffer));
    if (capture_log_sinks) {
        captured_fallback.assign(buffer, n);
        captured_serial.assign(buffer, n);
    }
    return n;
}

void syslog_send(const char* data, size_t len) {
    if (throw_syslog_sink == 1) throw std::bad_alloc();
    if (throw_syslog_sink == 2) throw 17;
    if (capture_log_sinks) {
        CHECK(!last_mutex || !last_mutex->held);
        captured_syslog.assign(data, len);
    }
}

tk::OtaVerificationState ota_verification_state() { return tk::OtaVerificationState::Stable; }

// Minimal deterministic NvsStorageAdapter implementation for the real safe_mode.cpp glue. The
// storage methods are fault seams: they can fail normally or throw, proving no exception can
// escape the boot boundary or the FreeRTOS task trampoline.
NvsStorageAdapter::NvsStorageAdapter(const char* namespace_name) : ns_(namespace_name) {}
NvsStorageAdapter::~NvsStorageAdapter() = default;
bool NvsStorageAdapter::initialize() { initialized_ = true; return true; }
bool NvsStorageAdapter::load(const std::string&, std::vector<uint8_t>&) { return false; }
bool NvsStorageAdapter::save(const std::string&, const std::vector<uint8_t>&) { return false; }
bool NvsStorageAdapter::remove(const std::string&) {
    ++remove_calls;
    if (throw_on_remove) throw std::bad_alloc();
    return remove_ok;
}
bool NvsStorageAdapter::load_str(const char*, std::string& out) {
    if (throw_on_load) throw std::bad_alloc();
    if (throw_other_on_load) throw std::runtime_error("load fixture");
    if (!load_ok) return false;
    out = stored_count;
    return true;
}
tk::NvsStringLoadState NvsStorageAdapter::load_str_state(const char*, std::string& out, bool) {
    if (throw_on_load) throw std::bad_alloc();
    if (throw_other_on_load) throw std::runtime_error("load fixture");
    if (!load_ok) {
        out.clear();
        return tk::NvsStringLoadState::Error;
    }
    if (!stored_present) {
        out.clear();
        return tk::NvsStringLoadState::Missing;
    }
    out = stored_count;
    return tk::NvsStringLoadState::Present;
}
bool NvsStorageAdapter::save_str(const char*, const std::string& value) {
    if (throw_on_save) throw std::bad_alloc();
    if (throw_other_on_save) throw std::runtime_error("save fixture");
    saved_counts.push_back(value);
    return save_ok;
}

static void test_iteration_fault_sink_boundary() {
    tk::IterationFaultLog state{};
    unsigned attempts = 0;
    uint32_t last_suppressed = 0;
    auto failing_sink = [&](uint32_t suppressed) {
        ++attempts;
        last_suppressed = suppressed;
        if (attempts == 1) throw std::bad_alloc();
        throw 42;
    };
    tk::report_iteration_fault(state, 0, failing_sink);
    CHECK(attempts == 1 && last_suppressed == 0);
    for (uint64_t time = 50; time < 30000; time += 50) {
        tk::report_iteration_fault(state, time, failing_sink);
        CHECK(attempts == 1); // state must advance even when the actual sink throws
    }
    tk::report_iteration_fault(state, 30000, failing_sink);
    CHECK(attempts == 2 && last_suppressed == 599);
    tk::report_iteration_fault(state, 30001, failing_sink);
    CHECK(attempts == 2);
    bool recovered_sink = false;
    tk::report_iteration_fault(state, 60000, [&](uint32_t suppressed) {
        recovered_sink = true;
        CHECK(suppressed == 1);
    });
    CHECK(recovered_sink);
}

static void test_runtime_admission_default_closed() {
    tk::RuntimeAdmissionGate booting;
    CHECK(!booting.vehicle_ready());
    CHECK(booting.action() == tk::RuntimeAdmissionAction::Wait);
    CHECK(booting.mark_ready());
    CHECK(booting.vehicle_ready());
    CHECK(booting.action() == tk::RuntimeAdmissionAction::Run);
    booting.mark_fatal();
    CHECK(!booting.vehicle_ready());
    CHECK(booting.action() == tk::RuntimeAdmissionAction::Stop);
    CHECK(!booting.mark_ready());

    tk::RuntimeAdmissionGate safe;
    CHECK(safe.mark_safe_mode());
    CHECK(!safe.vehicle_ready());
    CHECK(safe.action() == tk::RuntimeAdmissionAction::Stop);
    CHECK(!safe.mark_ready());
}

static void test_ota_confirm_ready_only_after_health_owner() {
    auto attempt_irreversible_mark = [](const tk::RuntimeAdmissionGate& admission,
                                        bool health_commit_owner, int& mark_calls) {
        if (!health_commit_owner || !admission.vehicle_ready()) return false;
        ++mark_calls;
        return true;
    };

    int mark_calls = 0;
    tk::RuntimeAdmissionGate booting;
    CHECK(!attempt_irreversible_mark(booting, true, mark_calls));

    tk::RuntimeAdmissionGate ready;
    CHECK(ready.mark_ready());
    CHECK(attempt_irreversible_mark(ready, true, mark_calls));
    CHECK(mark_calls == 1);
    CHECK(!attempt_irreversible_mark(ready, false, mark_calls));

    tk::RuntimeAdmissionGate safe_mode;
    CHECK(safe_mode.mark_safe_mode());
    CHECK(!attempt_irreversible_mark(safe_mode, true, mark_calls));

    tk::RuntimeAdmissionGate fatal;
    fatal.mark_fatal();
    CHECK(!attempt_irreversible_mark(fatal, true, mark_calls));
    CHECK(mark_calls == 1);
}

static void test_nimble_start_ack_runtime_matrix() {
    auto publish_runtime_ready = [](const tk::NimbleStartGate& host_start,
                                    int& ready_publications) {
        if (host_start.action() != tk::NimbleStartAction::Ready) return false;
        ++ready_publications;
        return true;
    };

    int ready_publications = 0;

    // Hidden host-task create failure: the void IDF wrapper returns, but no on_sync arrives.
    tk::NimbleStartGate create_failure;
    CHECK(create_failure.begin());
    CHECK(!publish_runtime_ready(create_failure, ready_publications));
    CHECK(create_failure.mark_timed_out());
    CHECK(create_failure.action() == tk::NimbleStartAction::Fail);
    CHECK(!publish_runtime_ready(create_failure, ready_publications));
    CHECK(!create_failure.acknowledge_sync());  // late sync after boot_fatal stays terminal
    CHECK(!publish_runtime_ready(create_failure, ready_publications));
    CHECK(ready_publications == 0);

    tk::NimbleStartGate acknowledged;
    CHECK(acknowledged.begin());
    CHECK(acknowledged.acknowledge_sync());
    CHECK(acknowledged.action() == tk::NimbleStartAction::Ready);
    CHECK(publish_runtime_ready(acknowledged, ready_publications));
    CHECK(ready_publications == 1);
    CHECK(!acknowledged.mark_timed_out());
}

struct FakeCoredumpStream {
    int fail_read_index{-1};
    int reads{0};
    int data_chunks{0};
    int terminating_chunks{0};

    bool read(std::size_t, char*, std::size_t) {
        const int index = reads++;
        return index != fail_read_index;
    }
    bool send(const char*, std::size_t size) {
        if (size == 0) {
            ++terminating_chunks;
        } else {
            ++data_chunks;
        }
        return true;
    }
};

static bool stream_coredump_fixture(std::size_t size, FakeCoredumpStream& transport) {
    char buffer[1024]{};
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t count =
            (size - offset) > sizeof(buffer) ? sizeof(buffer) : size - offset;
        if (!transport.read(offset, buffer, count)) return false;
        if (!transport.send(buffer, count)) return false;
        offset += count;
    }
    return transport.send(nullptr, 0);
}

static void test_coredump_read_failure_aborts_stream() {
    FakeCoredumpStream first_failure;
    first_failure.fail_read_index = 0;
    CHECK(!stream_coredump_fixture(2500, first_failure));
    CHECK(first_failure.reads == 1);
    CHECK(first_failure.data_chunks == 0);
    CHECK(first_failure.terminating_chunks == 0);

    FakeCoredumpStream nth_failure;
    nth_failure.fail_read_index = 2;
    CHECK(!stream_coredump_fixture(2500, nth_failure));
    CHECK(nth_failure.reads == 3);
    CHECK(nth_failure.data_chunks == 2);
    CHECK(nth_failure.terminating_chunks == 0);

    FakeCoredumpStream complete;
    CHECK(stream_coredump_fixture(2500, complete));
    CHECK(complete.reads == 3);
    CHECK(complete.data_chunks == 3);
    CHECK(complete.terminating_chunks == 1);
}

struct InjectedCriticalBoot {
    static constexpr int kSteps = 9;
    int fail_step{-1};
    int invalid_state_step{-1};
    int calls{0};
    int fatal_calls{0};
    int ready_publications{0};

    bool require_step(int step, bool invalid_state_is_idempotent = false) {
        ++calls;
        const int error = step == fail_step ? -1 : step == invalid_state_step ? 1 : 0;
        if (error == 0 || (invalid_state_is_idempotent && error == 1)) return true;
        ++fatal_calls;
        return false;
    }
};

static bool run_injected_critical_boot(InjectedCriticalBoot& boot) {
    // NVS; netif + event-loop substrate; station init/mode/config/start; AP HTTP + handlers.
    if (!boot.require_step(0)) return false;
    if (!boot.require_step(1, true)) return false;
    if (!boot.require_step(2, true)) return false;
    for (int step = 3; step < InjectedCriticalBoot::kSteps; ++step) {
        if (!boot.require_step(step)) return false;
    }
    ++boot.ready_publications;
    return true;
}

static void test_explicit_idf_boot_error_injection() {
    for (int fail_step = 0; fail_step < InjectedCriticalBoot::kSteps; ++fail_step) {
        InjectedCriticalBoot boot;
        boot.fail_step = fail_step;
        CHECK(!run_injected_critical_boot(boot));
        CHECK(boot.calls == fail_step + 1);
        CHECK(boot.fatal_calls == 1);
        CHECK(boot.ready_publications == 0);
    }

    InjectedCriticalBoot successful;
    CHECK(run_injected_critical_boot(successful));
    CHECK(successful.calls == InjectedCriticalBoot::kSteps);
    CHECK(successful.fatal_calls == 0);
    CHECK(successful.ready_publications == 1);

    for (int idempotent_step : {1, 2}) {
        InjectedCriticalBoot already_initialized;
        already_initialized.invalid_state_step = idempotent_step;
        CHECK(run_injected_critical_boot(already_initialized));
        CHECK(already_initialized.fatal_calls == 0);
        CHECK(already_initialized.ready_publications == 1);
    }
}

enum class OtaStatusFixtureResult { Current, Unavailable };

struct LazyOtaStatusLockFixture {
    int published_lock{0};
    int allocations{0};
    int deleted_candidates{0};
    int status_reads{0};
    int status_writes{0};
    int lock_takes{0};
    int lock_gives{0};
    bool allocation_fails{false};
    bool take_fails{false};
    bool publish_cas_winner{false};

    int ensure_lock() {
        int lock = published_lock;
        if (lock != 0) return lock;
        if (allocation_fails) return 0;
        const int candidate = ++allocations;
        if (publish_cas_winner) published_lock = 99;  // competing first caller wins the CAS
        if (published_lock != 0) {
            ++deleted_candidates;
            return published_lock;
        }
        published_lock = candidate;
        return candidate;
    }

    bool take(int lock) {
        if (lock == 0 || take_fails) return false;
        ++lock_takes;
        return true;
    }

    OtaStatusFixtureResult read() {
        const int lock = ensure_lock();
        if (!take(lock)) return OtaStatusFixtureResult::Unavailable;
        ++status_reads;  // the only modeled s_status access, necessarily under the guard
        ++lock_gives;
        return OtaStatusFixtureResult::Current;
    }

    bool write() {
        const int lock = ensure_lock();
        if (!take(lock)) return false;
        ++status_writes;
        ++lock_gives;
        return true;
    }
};

static void test_ota_status_lock_failure_matrix() {
    LazyOtaStatusLockFixture read_alloc_failure;
    read_alloc_failure.allocation_fails = true;
    CHECK(read_alloc_failure.read() == OtaStatusFixtureResult::Unavailable);
    CHECK(read_alloc_failure.status_reads == 0);

    LazyOtaStatusLockFixture read_take_failure;
    read_take_failure.take_fails = true;
    CHECK(read_take_failure.read() == OtaStatusFixtureResult::Unavailable);
    CHECK(read_take_failure.status_reads == 0);

    LazyOtaStatusLockFixture write_alloc_failure;
    write_alloc_failure.allocation_fails = true;
    CHECK(!write_alloc_failure.write());
    CHECK(write_alloc_failure.status_writes == 0);

    LazyOtaStatusLockFixture write_take_failure;
    write_take_failure.take_fails = true;
    CHECK(!write_take_failure.write());
    CHECK(write_take_failure.status_writes == 0);

    LazyOtaStatusLockFixture cas_loser;
    cas_loser.publish_cas_winner = true;
    CHECK(cas_loser.read() == OtaStatusFixtureResult::Current);
    CHECK(cas_loser.published_lock == 99);
    CHECK(cas_loser.allocations == 1);
    CHECK(cas_loser.deleted_candidates == 1);
    CHECK(cas_loser.status_reads == 1);
    CHECK(cas_loser.lock_takes == 1);
    CHECK(cas_loser.lock_gives == 1);

    LazyOtaStatusLockFixture shared;
    CHECK(shared.write());
    CHECK(shared.read() == OtaStatusFixtureResult::Current);
    CHECK(shared.allocations == 1);
    CHECK(shared.status_writes == 1);
    CHECK(shared.status_reads == 1);
    CHECK(shared.lock_takes == shared.lock_gives);
}

struct SyslogQueuePublicationFixture {
    explicit SyslogQueuePublicationFixture(int& observed_deletes)
        : observed_deletes_(&observed_deletes) {}

    tk::SyslogStartGate gate;
    int queue_storage{7};
    int* published{nullptr};
    bool local_owner{true};
    int deletes{0};
    int sends{0};
    int consumes{0};

    ~SyslogQueuePublicationFixture() {
        if (local_owner) {
            ++deletes;
            ++*observed_deletes_;
        }
    }

    bool send_from_log_hook() {
        int* const captured = published;
        if (!captured) return false;
        CHECK(*captured == 7);
        ++sends;
        return true;
    }

    tk::SyslogStartAction task_step() {
        const tk::SyslogStartAction action = gate.action();
        if (action == tk::SyslogStartAction::Run) {
            CHECK(published == &queue_storage);
            ++consumes;
        }
        return action;
    }

private:
    int* observed_deletes_;
};

static void test_syslog_queue_publication_lifetime() {
    // Disabled and allocation-failure states never publish to the global log hook. Local
    // ownership may therefore delete safely without any sender being able to capture the handle.
    int allocation_failure_deletes = 0;
    {
        SyslogQueuePublicationFixture allocation_failure(allocation_failure_deletes);
        CHECK(!allocation_failure.send_from_log_hook());
        CHECK(allocation_failure.gate.action() == tk::SyslogStartAction::Wait);
    }
    CHECK(allocation_failure_deletes == 1);

    // xTaskCreate may schedule the consumer before returning. It must wait while the queue remains
    // local. On create failure cancellation becomes terminal before RAII deletes that local queue;
    // sends before and after the cancellation both observe no publication.
    int task_failure_deletes = 0;
    {
        SyslogQueuePublicationFixture failed_start(task_failure_deletes);
        CHECK(failed_start.gate.begin());
        CHECK(failed_start.task_step() == tk::SyslogStartAction::Wait);
        CHECK(!failed_start.send_from_log_hook());
        CHECK(failed_start.gate.cancel());
        CHECK(failed_start.task_step() == tk::SyslogStartAction::Cancel);
        CHECK(!failed_start.send_from_log_hook());
        CHECK(failed_start.deletes == 0);  // deletion happens only as local ownership unwinds
    }
    CHECK(task_failure_deletes == 1);

    // Success publishes a boot-lifetime queue before opening the consumer gate, then releases
    // local deletion authority. Every sender capture is therefore paired with a queue that cannot
    // be deleted until reboot.
    int success_deletes = 0;
    {
        SyslogQueuePublicationFixture success(success_deletes);
        CHECK(success.gate.begin());
        CHECK(success.task_step() == tk::SyslogStartAction::Wait);
        success.published = &success.queue_storage;
        success.local_owner = false;
        CHECK(success.send_from_log_hook());  // publication may precede the task's Run observation
        CHECK(success.gate.commit());
        CHECK(success.task_step() == tk::SyslogStartAction::Run);
        CHECK(success.send_from_log_hook());
        CHECK(success.sends == 2);
        CHECK(success.consumes == 1);
        CHECK(success.deletes == 0);
    }
    CHECK(success_deletes == 0);
}

static void test_dynamic_ble_host_health_blocks_ota_commit() {
    auto attempt_commit = [](bool health_commit_owner, bool runtime_ready,
                             bool host_synced, std::uint32_t host_reset_count,
                             bool heap_healthy, int& mark_calls) {
        if (!health_commit_owner || !runtime_ready || !host_synced ||
            host_reset_count != 0 || !heap_healthy) return false;
        ++mark_calls;
        return true;
    };

    int timed_mark_calls = 0;
    int explicit_mark_calls = 0;
    bool host_synced = true;
    std::uint32_t host_reset_count = 0;
    CHECK(attempt_commit(true, true, host_synced, host_reset_count, true, timed_mark_calls));
    CHECK(attempt_commit(true, true, host_synced, host_reset_count, true, explicit_mark_calls));

    host_synced = false;  // on_reset, no later on_sync
    host_reset_count = 1;
    CHECK(!attempt_commit(true, true, host_synced, host_reset_count, true, timed_mark_calls));
    CHECK(!attempt_commit(true, true, host_synced, host_reset_count, true, explicit_mark_calls));
    CHECK(timed_mark_calls == 1);
    CHECK(explicit_mark_calls == 1);

    host_synced = true;  // quick resync cannot erase sticky reset evidence
    CHECK(!attempt_commit(true, true, host_synced, host_reset_count, true, timed_mark_calls));
    CHECK(!attempt_commit(true, true, host_synced, host_reset_count, true, explicit_mark_calls));
    CHECK(timed_mark_calls == 1);
    CHECK(explicit_mark_calls == 1);

    host_reset_count = 0;  // only a fresh stable boot is eligible again
    CHECK(attempt_commit(true, true, host_synced, host_reset_count, true, timed_mark_calls));
    CHECK(attempt_commit(true, true, host_synced, host_reset_count, true, explicit_mark_calls));
    CHECK(timed_mark_calls == 2);
    CHECK(explicit_mark_calls == 2);
}

static void test_ping_probe_lifecycle() {
    const esp_ping_config_t config{};

    {
        tk::PingProbeControl control{};
        control.done = xSemaphoreCreateBinary();
        reset_ping_capture(PingScenario::SetupFail);
        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::Unavailable);
        CHECK(ping_new_calls == 1);
        CHECK(ping_start_calls == 0);
        CHECK(ping_delete_calls == 0);
        CHECK(control.session == nullptr);
        CHECK(control.generation.active() == 0);
        vSemaphoreDelete(control.done);
    }

    // If start fails and even deleting that unstarted SDK session fails, ownership must stay
    // persistent. Re-entry retries the exact handle and may neither allocate nor advance a
    // generation until cleanup succeeds.
    {
        tk::PingProbeControl control{};
        control.done = xSemaphoreCreateBinary();
        reset_ping_capture(PingScenario::StartDeleteFail);
        ping_delete_failures_remaining = 2;
        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::Unavailable);
        CHECK(ping_new_calls == 1);
        CHECK(ping_start_calls == 1);
        CHECK(ping_delete_calls == 1);
        CHECK(control.session != nullptr);
        CHECK(!control.session_started);
        HostPingSession* const retained_session = control.session;
        const std::uint32_t retained_generation = control.generation.active();
        CHECK(retained_generation != 0);

        ping_scenario = PingScenario::Reply;
        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::PendingEnd);
        CHECK(ping_new_calls == 1);
        CHECK(ping_start_calls == 1);
        CHECK(ping_delete_calls == 2);
        CHECK(control.session == retained_session);
        CHECK(control.generation.active() == retained_generation);

        // The third delete attempt releases the retained unstarted generation; only then may
        // this same serial invocation allocate, run and retire a replacement generation.
        CHECK(tk::ping_probe_run(control, config, 20, 5) == tk::PingProbeResult::Reply);
        CHECK(ping_new_calls == 2);
        CHECK(ping_start_calls == 2);
        CHECK(ping_delete_calls == 4);
        CHECK(control.session == nullptr);
        CHECK(control.generation.active() == 0);
        vSemaphoreDelete(control.done);
    }

    {
        tk::PingProbeControl control{};
        control.done = xSemaphoreCreateBinary();
        reset_ping_capture(PingScenario::StartFail);
        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::Unavailable);
        CHECK(ping_start_calls == 1);
        CHECK(ping_stop_calls == 0);
        CHECK(ping_delete_calls == 1);
        CHECK(control.session == nullptr);
        CHECK(control.generation.active() == 0);
        vSemaphoreDelete(control.done);
    }

    {
        tk::PingProbeControl control{};
        control.done = xSemaphoreCreateBinary();
        reset_ping_capture(PingScenario::Reply);
        CHECK(tk::ping_probe_run(control, config, 20, 5) == tk::PingProbeResult::Reply);
        CHECK(ping_new_calls == 1);
        CHECK(ping_start_calls == 1);
        CHECK(ping_stop_calls == 0);
        CHECK(ping_delete_calls == 1);
        CHECK(control.session == nullptr);
        CHECK(control.generation.active() == 0);
        vSemaphoreDelete(control.done);
    }

    {
        tk::PingProbeControl control{};
        control.done = xSemaphoreCreateBinary();
        reset_ping_capture(PingScenario::NoReply);
        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::NoReply);
        CHECK(ping_delete_calls == 1);
        vSemaphoreDelete(control.done);
    }

    // An ended ping task is not proof that its reply counter was readable. Profile failure is an
    // unknown measurement, never a fabricated zero-reply outage; its exact generation is still
    // deleted/retired so the next probe can start normally.
    {
        tk::PingProbeControl control{};
        control.done = xSemaphoreCreateBinary();
        reset_ping_capture(PingScenario::ProfileFail);
        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::Unavailable);
        CHECK(ping_new_calls == 1);
        CHECK(ping_delete_calls == 1);
        CHECK(control.session == nullptr);
        CHECK(control.generation.active() == 0);

        ping_scenario = PingScenario::Reply;
        CHECK(tk::ping_probe_run(control, config, 20, 5) == tk::PingProbeResult::Reply);
        CHECK(ping_new_calls == 2);
        CHECK(ping_delete_calls == 2);
        CHECK(control.generation.active() == 0);
        vSemaphoreDelete(control.done);
    }

    auto gateway_failure = [](tk::PingProbeResult result) {
        return result == tk::PingProbeResult::NoReply;
    };
    CHECK(!gateway_failure(tk::PingProbeResult::Reply));
    CHECK(gateway_failure(tk::PingProbeResult::NoReply));
    CHECK(!gateway_failure(tk::PingProbeResult::Unavailable));
    CHECK(!gateway_failure(tk::PingProbeResult::PendingEnd));

    // A timed-out session is stopped but not deleted. Re-entry remains quarantined until the
    // exact generation's on_ping_end arrives; an older generation cannot wake or retire it.
    {
        tk::PingProbeControl control{};
        control.done = xSemaphoreCreateBinary();

        reset_ping_capture(PingScenario::Reply);
        CHECK(tk::ping_probe_run(control, config, 20, 5) == tk::PingProbeResult::Reply);
        tk::PingProbeCallbackArgs stale_generation = captured_ping_args;

        reset_ping_capture(PingScenario::TimeoutLateEnd);
        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::PendingEnd);
        CHECK(ping_new_calls == 1);
        CHECK(ping_start_calls == 1);
        CHECK(ping_stop_calls == 1);
        CHECK(ping_delete_calls == 0);
        CHECK(control.session != nullptr);
        const std::uint32_t pending_generation = control.generation.active();
        CHECK(pending_generation != 0);
        CHECK(!control.generation.ended(pending_generation));

        tk::ping_probe_on_end(control.session, &stale_generation);
        CHECK(!control.generation.ended(pending_generation));
        CHECK(ping_delete_calls == 0);

        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::PendingEnd);
        CHECK(ping_new_calls == 1);
        CHECK(ping_start_calls == 1);
        CHECK(ping_delete_calls == 0);

        HostPingSession* const pending_session = control.session;
        CHECK(pending_session == last_ping_session);
        pending_session->replies = 1;
        pending_session->callbacks.on_ping_end(
            pending_session, pending_session->callbacks.cb_args);
        CHECK(control.generation.ended(pending_generation));

        ping_scenario = PingScenario::NoReply;
        CHECK(tk::ping_probe_run(control, config, 20, 5) ==
              tk::PingProbeResult::NoReply);
        CHECK(ping_new_calls == 2);
        CHECK(ping_start_calls == 2);
        CHECK(ping_delete_calls == 2);
        CHECK(control.session == nullptr);
        CHECK(control.generation.active() == 0);
        vSemaphoreDelete(control.done);
    }
}

static bool complete_diag_http_fixture(DiagDumpResult dump, bool final_flush_ok,
                                       int& terminating_chunks) {
    if (dump != DiagDumpResult::Complete || !final_flush_ok) return false;
    ++terminating_chunks;
    return true;
}

static void test_diag_http_completion_matrix() {
    int terminating_chunks = 0;
    CHECK(!complete_diag_http_fixture(DiagDumpResult::SinkFailed, true, terminating_chunks));
    CHECK(terminating_chunks == 0);
    CHECK(!complete_diag_http_fixture(
        DiagDumpResult::SnapshotInvalidated, true, terminating_chunks));
    CHECK(terminating_chunks == 0);
    CHECK(!complete_diag_http_fixture(DiagDumpResult::Complete, false, terminating_chunks));
    CHECK(terminating_chunks == 0);
    CHECK(complete_diag_http_fixture(DiagDumpResult::Complete, true, terminating_chunks));
    CHECK(terminating_chunks == 1);
}

static void test_actual_logger_stack_sampling() {
    // The complete actual diag_log.cpp helper is compiled against these SDK doubles.
    // Only the added sampling chain is tested for unsafe contexts; this does not claim
    // that the pre-existing ring or Serial/syslog sink accepts arbitrary ISR logging.
    CHECK(last_mutex != nullptr && !last_mutex->held);
    capture_log_sinks = true;
    track_log_stack_queries = true;
    const auto before_min = tk::stack_watch_min_free_bytes(tk::StackWatch::DiagLogCallback);
    for (int refusal : {0, 1, 2, 3, 4}) {
        fake_isr = refusal == 0;
        fake_can_yield = refusal != 1;
        fake_scheduler_state = refusal == 2 ? taskSCHEDULER_NOT_STARTED :
                               refusal == 3 ? taskSCHEDULER_SUSPENDED : taskSCHEDULER_RUNNING;
        fake_current_task = refusal == 4 ? nullptr : &current_task_token;
        log_stack_trace_size = 0; log_stack_trace[0] = '\0';
        const auto queries = stack_queries;
        emit_log("ordinary guarded diagnostic %d\n", refusal);
        CHECK(std::strcmp(log_stack_trace, refusal == 0 ? "I" : refusal == 1 ? "IY" :
                          refusal == 4 ? "IYSC" : "IYS") == 0);
        CHECK(stack_queries == queries);
        CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::DiagLogCallback) == before_min);
        CHECK(captured_serial == captured_syslog);
    }
    fake_isr = false; fake_can_yield = true; fake_scheduler_state = taskSCHEDULER_RUNNING;
    fake_current_task = &current_task_token;
    fake_stack_free = 128; // A real observation retains a present zero lower bound.
    for (int sink : {0, 1, 2}) {
        for (int fault : {0, 1, 2}) {
            if (sink == 0 && fault != 0) continue;
            throw_serial_sink = sink == 1 ? fault : 0;
            throw_syslog_sink = sink == 2 ? fault : 0;
            log_stack_trace_size = 0; log_stack_trace[0] = '\0';
            const auto queries = stack_queries;
            bool caught = false;
            try { emit_log("ordinary sink-unwind diagnostic\n"); }
            catch (...) { caught = true; }
            CHECK(caught == (fault != 0));
            CHECK(std::strcmp(log_stack_trace, "IYSCH") == 0);
            CHECK(stack_queries == queries + 1);
            CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::DiagLogCallback) == 0u);
            CHECK(!last_mutex->held);
        }
    }
    throw_serial_sink = 0; throw_syslog_sink = 0;
    log_stack_trace_size = 0; log_stack_trace[0] = '\0';
    emit_log("E (1) mqtt_client: Error parse uri (1) = mqtt://example:secret@broker.invalid\n");
    CHECK(std::strcmp(log_stack_trace, "IYSCH") == 0);
    CHECK(captured_serial == tk::kDiagMqttUriErrorLine);
    CHECK(captured_syslog == tk::kDiagMqttUriErrorLine);
    CHECK(!last_mutex->held);
    // Sampling itself has no allocator/log/sink call. The ordinary sink below is passive,
    // and its capture strings are disabled so allocation denial targets the actual helper.
    capture_log_sinks = false;
    fail_global_allocation = true;
    const auto queries = stack_queries;
    emit_log("no-allocation diagnostic sample\n");
    fail_global_allocation = false;
    CHECK(stack_queries == queries + 1);
    track_log_stack_queries = false;
    diag_log_clear();
}

int main() {
    test_actual_guard_unwind_and_stack_minima();
    checks += test_coredump_hash();
#ifdef TEST_OTA_ADMISSION_ADAPTER
    checks += test_ota_admission();
#endif
#ifdef TEST_BACKGROUND_POLL_ADAPTER
    checks += test_background_poll();
#endif
    log_sink = discard_log;

    test_iteration_fault_sink_boundary();
    test_runtime_admission_default_closed();
    test_ota_confirm_ready_only_after_health_owner();
    test_nimble_start_ack_runtime_matrix();
    test_coredump_read_failure_aborts_stream();
    test_explicit_idf_boot_error_injection();
    test_syslog_queue_publication_lifetime();
    test_ota_status_lock_failure_matrix();
    test_dynamic_ble_host_health_blocks_ota_commit();
    test_ping_probe_lifecycle();
    test_diag_http_completion_matrix();

    // Real diag_log.cpp: every sink call must run after the ring mutex has been released. A
    // throwing or early-stopping network sink must leave the shared producer lock usable.
    const unsigned previous_installs = log_hook_installs;
    last_mutex = nullptr;
    fail_diag_mutex_create = true;
    capture_log_sinks = true;
    inject_hook_reentry = true;
    inject_mutex_reentry = true;
    diag_log_init();
    CHECK(hook_reentries == 1 && mutex_reentries == 1);
    CHECK(captured_fallback == tk::kDiagMqttUriErrorLine);
    inject_hook_reentry = false;
    emit_log("E (1) mqtt_client: Error parse uri (1) = mqtt://example:secret@broker.invalid\n");
    capture_log_sinks = false;
    CHECK(captured_serial == tk::kDiagMqttUriErrorLine);
    CHECK(captured_syslog == tk::kDiagMqttUriErrorLine);
    CHECK(diag_log_dump_chunks([](const char*, size_t) { return true; }) == DiagDumpResult::SnapshotInvalidated);
    diag_log_init();  // another failed retry must not replace s_prev with our own hook
    CHECK(log_hook_installs == previous_installs + 1);
    fail_diag_mutex_create = false;
    capture_log_sinks = true;
    diag_log_init();
    CHECK(captured_serial == tk::kDiagMqttUriErrorLine && captured_syslog == tk::kDiagMqttUriErrorLine);
    CHECK(mutex_reentries == 3);
    inject_mutex_reentry = false;
    capture_log_sinks = false;
    diag_log_init();
    CHECK(log_hook_installs == previous_installs + 1);
    CHECK(last_mutex != nullptr);
    test_actual_logger_stack_sampling();
    diag_log_clear();
    const auto drops_before = diag_log_dropped_lines();
    last_mutex->held = true;
    emit_log("contended capture must not block\n");
    CHECK(last_mutex_wait == 0);
    CHECK(diag_log_dropped_lines() == drops_before + 1);
    last_mutex->held = false;
    diag_log_clear();
    CHECK(diag_log_dropped_lines() == drops_before + 1);
    emit_log("first %d\n", 1);
    CHECK(diag_log_dropped_lines() == drops_before + 1);

    std::string dump;
    const DiagDumpResult first_dump = diag_log_dump_chunks([&](const char* data, size_t len) {
        CHECK(!last_mutex->held);
        dump.append(data, len);
        return true;
    });
    CHECK(first_dump == DiagDumpResult::Complete);
    CHECK(dump == "first 1\n");
    CHECK(!last_mutex->held);

    bool threw = false;
    try {
        diag_log_dump_chunks([&](const char*, size_t) -> bool {
            CHECK(!last_mutex->held);
            throw std::bad_alloc();
        });
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!last_mutex->held);

    // Prove the mutex remains live after unwinding, and false stops before another chunk.
    diag_log_clear();
    for (int i = 0; i < 4; ++i) emit_log("%080d\n", i);
    int chunks = 0;
    const DiagDumpResult first_sink_failure = diag_log_dump_chunks([&](const char*, size_t) {
        CHECK(!last_mutex->held);
        ++chunks;
        return false;
    });
    CHECK(first_sink_failure == DiagDumpResult::SinkFailed);
    CHECK(chunks == 1);
    CHECK(!last_mutex->held);
    diag_log_clear();

    // A later transport failure is equally terminal: no additional snapshot chunk is delivered.
    for (int i = 0; i < 10; ++i) emit_log("%080d\n", i);
    int nth_failure_chunks = 0;
    const DiagDumpResult nth_sink_failure = diag_log_dump_chunks([&](const char*, size_t) {
        CHECK(!last_mutex->held);
        ++nth_failure_chunks;
        return nth_failure_chunks != 3;
    });
    CHECK(nth_sink_failure == DiagDumpResult::SinkFailed);
    CHECK(nth_failure_chunks == 3);
    CHECK(!last_mutex->held);
    diag_log_clear();

    // Fill and wrap the real ring. While the first chunk is in the unlocked sink, append enough
    // data to overtake the next unread byte. The dump must stop before mixing the new prefix with
    // the old snapshot tail (which could otherwise bypass line-marker redaction).
    const std::string noisy_line(400, 'N');
    for (int i = 0; i < 65; ++i) emit_log("%s", noisy_line.c_str());
    int overwrite_chunks = 0;
    const DiagDumpResult overwrite_result = diag_log_dump_chunks([&](const char*, size_t) {
        CHECK(!last_mutex->held);
        ++overwrite_chunks;
        if (overwrite_chunks == 1) {
            emit_log("%s", noisy_line.c_str());
            emit_log("%s", noisy_line.c_str());
        }
        return true;
    });
    CHECK(overwrite_result == DiagDumpResult::SnapshotInvalidated);
    CHECK(overwrite_chunks == 1);
    CHECK(!last_mutex->held);

    // A wrapped byte ring can begin in the middle of a sensitive value after its marker has
    // already been overwritten. Reproduce that exact shape, then execute the same two production
    // seams as /diag?redact=1: discard the untrusted first fragment in diag_log.cpp and redact all
    // complete retained lines with the fixed-buffer redactor.
    diag_log_clear();
    const std::string sensitive_line =
        "I (1) main: VIN: 5YJ3E1EA7KF000316  BLE MAC: aa:bb:cc:dd:ee:ff  "
        "Board MAC: 02:00:00:32:55:20\n";
    for (int i = 0; i < 400; ++i) emit_log("%s", sensitive_line.c_str());

    std::string raw_wrapped;
    size_t first_newline = std::string::npos;
    bool markerless_secret_prefix = false;
    for (size_t shift = 0; shift < sensitive_line.size(); ++shift) {
        raw_wrapped.clear();
        diag_log_dump_chunks([&](const char* data, size_t len) {
            raw_wrapped.append(data, len);
            return true;
        });
        first_newline = raw_wrapped.find('\n');
        if (first_newline != std::string::npos) {
            const std::string prefix = raw_wrapped.substr(0, first_newline);
            markerless_secret_prefix =
                prefix.find("5YJ3E1EA7KF000316") != std::string::npos &&
                prefix.find("main: VIN: ") == std::string::npos;
            if (markerless_secret_prefix) break;
        }
        emit_log("X");  // advance the full ring's oldest byte by exactly one
    }
    CHECK(markerless_secret_prefix);
    CHECK(first_newline != std::string::npos);

    std::string boundary_safe;
    diag_log_dump_chunks([&](const char* data, size_t len) {
        CHECK(!last_mutex->held);
        boundary_safe.append(data, len);
        return true;
    }, DiagDumpStart::AfterWrappedLineBoundary);
    CHECK(boundary_safe == raw_wrapped.substr(first_newline + 1));
    CHECK(boundary_safe.find("5YJ3E1EA7KF000316") != std::string::npos);

    std::string redacted_report;
    size_t line_start = 0;
    char redacted_line[tk::diag_redacted_capacity(512)]{};
    while (line_start < boundary_safe.size()) {
        const size_t newline = boundary_safe.find('\n', line_start);
        const size_t line_end = newline == std::string::npos
            ? boundary_safe.size()
            : newline + 1;
        const std::string_view line(boundary_safe.data() + line_start,
                                    line_end - line_start);
        const tk::FixedDiagRedaction result = tk::redact_diag_line_fixed(
            line, redacted_line, sizeof(redacted_line));
        CHECK(result.safe);
        redacted_report.append(redacted_line, result.size);
        line_start = line_end;
    }
    CHECK(redacted_report.find("5YJ3E1EA7KF000316") == std::string::npos);
    CHECK(redacted_report.find("aa:bb:cc:dd:ee:ff") == std::string::npos);
    CHECK(redacted_report.find("02:00:00:32:55:20") != std::string::npos);
    CHECK(!last_mutex->held);

    // A clear starts a new logical ring even though old bytes remain in the static array. Detect
    // that epoch change as well and never stream the cleared tail.
    diag_log_clear();
    for (int i = 0; i < 4; ++i) emit_log("%s", noisy_line.c_str());
    int clear_chunks = 0;
    const DiagDumpResult clear_result = diag_log_dump_chunks([&](const char*, size_t) {
        ++clear_chunks;
        if (clear_chunks == 1) diag_log_clear();
        return true;
    });
    CHECK(clear_result == DiagDumpResult::SnapshotInvalidated);
    CHECK(clear_chunks == 1);
    CHECK(!last_mutex->held);
    diag_log_clear();

    auto export_diag_capture = [&]() {
    std::string exported;
    tk::DiagLineFrame frame;
    char input[288]{};
    char output[tk::diag_redacted_capacity(sizeof(input))]{};
    auto flush = [&](size_t size) {
        const auto redacted = tk::redact_diag_line_fixed({input, size}, output, sizeof(output));
        CHECK(redacted.safe); exported.append(output, redacted.size);
    };
    CHECK(diag_log_dump_chunks([&](const char* data, size_t len) {
        CHECK(!last_mutex->held);
        for (size_t i = 0; i < len; ++i) {
            const auto step = tk::diag_line_step(frame, data[i], sizeof(input));
            if (step.action == tk::DiagLineAction::Append) input[step.size] = data[i];
            else if (step.action == tk::DiagLineAction::EmitLine) flush(step.size);
            else if (step.action == tk::DiagLineAction::EmitOverlong) exported += tk::kRedacted;
        }
        return true;
    }, DiagDumpStart::AfterWrappedLineBoundary) == DiagDumpResult::Complete);
    if (frame.size) flush(frame.size);
        return exported;
    };
    // Real formatted-callback -> ring -> chunk framing -> fixed redactor. Embedded CR/LF,
    // including a forged log prefix, must not expose a second markerless value. Private Serial
    // and Syslog receive the original bytes; only the ring copy is normalized.
    for (const char* network_record : {
            "I (1) wifi:connected with example",
            "I (1) wifi:<ba-add>idx:0 (ifx:0, 02:00:00:00:00:01), tid:6, ssn:0, winSize:64"})
    for (const char* ending : {"\n", "\r\n"})
    for (bool wrapped : {false, true}) for (bool truncated : {false, true}) {
        diag_log_clear();
        const char* private_tail = std::string_view(network_record).find("<ba-add>") !=
            std::string_view::npos ? "block-ack-private-tail" : "private-tail";
        std::string record = std::string(network_record) + "\r\nI (0) fake: " + private_tail;
        if (truncated) record += std::string(240, 'X');
        record += ending;
        capture_log_sinks = true;
        captured_serial.clear(); captured_syslog.clear();
        emit_log("%s", record.c_str());
        capture_log_sinks = false;
        CHECK(captured_serial == record);
        CHECK(captured_syslog == record.substr(0, 255));
        if (truncated) emit_log("\n");  // a later callback terminates the truncated record
        if (wrapped) {
            for (int i = 0; i < 350; ++i) {
                emit_log("%s", record.c_str());
                if (truncated) emit_log("\n");
            }
        }
        emit_log("I (2) app: after\n");
        std::string raw;
        CHECK(diag_log_dump_chunks([&](const char* data, size_t len) {
            CHECK(!last_mutex->held); raw.append(data, len); return true;
        }, DiagDumpStart::AfterWrappedLineBoundary) == DiagDumpResult::Complete);
        CHECK(raw.find(std::string(network_record) + "  I (0) fake: " + private_tail) != std::string::npos);
        CHECK(raw.find("\nI (0) fake:") == std::string::npos);
        const std::string exported = export_diag_capture();
        CHECK(exported.find("example") == std::string::npos);
        CHECK(exported.find("02:00:00:00:00:01") == std::string::npos);
        CHECK(exported.find("<ba-add>") == std::string::npos);
        CHECK(exported.find("block-ack-private-tail") == std::string::npos);
        CHECK(exported.find("private-tail") == std::string::npos);
        CHECK(exported.find(tk::kRedacted) != std::string::npos);
    }
    for (bool truncated : {false, true}) {
        diag_log_clear();
        std::string record = "E (123) mqtt_client: Error parse uri (non-hexadecimal data after %) = mqtt://example:secret@broker.invalid\nI (0) fake: private-tail";
        if (truncated) record += std::string(240, 'X');
        record += "\n";
        captured_serial.clear(); captured_syslog.clear(); capture_log_sinks = true;
        emit_log("%s", record.c_str());
        capture_log_sinks = false;
        CHECK(captured_serial == tk::kDiagMqttUriErrorLine);
        CHECK(captured_syslog == tk::kDiagMqttUriErrorLine);
        std::string raw;
        CHECK(diag_log_dump_chunks([&](const char* data, size_t len) { raw.append(data, len); return true; }) == DiagDumpResult::Complete);
        CHECK(raw == tk::kDiagMqttUriErrorLine);
        CHECK(export_diag_capture().find("secret") == std::string::npos);
    }
    // A forged MQTT error inside another record must not rewrite its private raw sinks.
    diag_log_clear();
    const std::string foreign = "I (123) wifi:connected with E (1) mqtt_client: Error parse uri (1) = example\n";
    captured_serial.clear(); captured_syslog.clear(); capture_log_sinks = true;
    emit_log("%s", foreign.c_str());
    capture_log_sinks = false;
    CHECK(captured_serial == foreign && captured_syslog == foreign);

    // No artificial LF: a truncated callback followed directly by an ordinary complete record,
    // or an entirely unterminated wrapped ring, may discard the whole export. It must never
    // emit the markerless value or flush a partial secret as if it were a complete line.
    for (bool wrapped : {false, true}) for (bool next_record : {false, true}) {
        diag_log_clear();
        const std::string long_record =
            "I (1) wifi:connected with example\nI (0) fake: private-tail" + std::string(240, 'X');
        for (unsigned i = 0; i < (wrapped ? 350u : 1u); ++i) emit_log("%s", long_record.c_str());
        if (next_record) emit_log("I (2) app: complete next record\n");
        const auto exported = export_diag_capture();
        CHECK(exported.find("example") == std::string::npos);
        CHECK(exported.find("private-tail") == std::string::npos);
        CHECK(exported.find("fake:") == std::string::npos);
        if (wrapped) CHECK(exported.empty());
    }
    diag_log_clear();
    const std::string ordinary = "I (3) app: ordinary\nsecond line\n";
    emit_log("%s", ordinary.c_str());
    std::string unchanged;
    CHECK(diag_log_dump_chunks([&](const char* data, size_t len) {
        unchanged.append(data, len); return true;
    }) == DiagDumpResult::Complete);
    CHECK(unchanged == ordinary);
    diag_log_clear();

    // Temporary MQTT probe ownership is deterministic at every acquire stage. In particular, a
    // throw after semaphore/client acquisition joins a started callback task before destroying
    // its client and only then deletes the semaphore that the callback can still signal.
    FakeMqttProbeOps::events.clear();
    { tk::MqttProbeResourceOwner<FakeMqttProbeOps> owner; }
    CHECK(FakeMqttProbeOps::events.empty());

    FakeMqttProbeOps::events.clear();
    {
        tk::MqttProbeResourceOwner<FakeMqttProbeOps> owner;
        owner.sem = reinterpret_cast<void*>(1);
    }
    CHECK((FakeMqttProbeOps::events == std::vector<std::string>{"delete_sem"}));

    FakeMqttProbeOps::events.clear();
    {
        tk::MqttProbeResourceOwner<FakeMqttProbeOps> owner;
        owner.sem = reinterpret_cast<void*>(1);
        owner.client = reinterpret_cast<void*>(2);
    }
    CHECK((FakeMqttProbeOps::events ==
           std::vector<std::string>{"destroy", "delete_sem"}));

    FakeMqttProbeOps::events.clear();
    try {
        tk::MqttProbeResourceOwner<FakeMqttProbeOps> owner;
        owner.sem = reinterpret_cast<void*>(1);
        owner.client = reinterpret_cast<void*>(2);
        owner.started = true;
        throw std::bad_alloc();
    } catch (const std::bad_alloc&) {
    }
    CHECK((FakeMqttProbeOps::events ==
           std::vector<std::string>{"stop", "destroy", "delete_sem"}));

    FakeMqttProbeOps::events.clear();
    {
        tk::MqttProbeResourceOwner<FakeMqttProbeOps> owner;
        owner.sem = reinterpret_cast<void*>(1);
        owner.client = reinterpret_cast<void*>(2);
        owner.started = false;  // an explicit successful stop already joined the callback task
    }
    CHECK((FakeMqttProbeOps::events ==
           std::vector<std::string>{"destroy", "delete_sem"}));

    NvsStorageAdapter storage("tesla_cfg");

    // Real safe_mode_begin: ordinary progression still works and persists the exact next count.
    reset_storage("3");
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
    CHECK(saved_counts.size() == 1 && saved_counts[0] == "4");
    CHECK(tk::safe_mode_active());

    for (unsigned prior : {0u, 1u, 2u, 3u}) {
        const std::string before = std::to_string(prior);
        reset_storage(before.c_str());
        CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing) == (prior == 3));
        CHECK(saved_counts.size() == 1);
        CHECK(saved_counts[0] == std::to_string(prior + 1));
        CHECK(tk::safe_mode_active() == (prior == 3));
    }

    reset_storage("3");
    CHECK(!tk::safe_mode_begin(storage, tk::BootCounterCause::Clean, tk::RebootReasonState::Missing));
    CHECK(saved_counts.size() == 1 && saved_counts[0] == "0");
    CHECK(!tk::safe_mode_active());

    // An intentional reset cannot bypass failed counter storage or hide a malformed record.
    reset_storage("3");
    load_ok = false;
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Clean, tk::RebootReasonState::Missing));
    CHECK(saved_counts.empty());
    CHECK(tk::safe_mode_active());

    reset_storage("broken");
    stored_present = true;
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Clean, tk::RebootReasonState::Missing));
    CHECK(saved_counts.empty());
    CHECK(tk::safe_mode_active());

    reset_storage("3");
    save_ok = false;
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Clean, tk::RebootReasonState::Missing));
    CHECK(saved_counts.size() == 1 && saved_counts[0] == "0");
    CHECK(tk::safe_mode_active());

    for (bool fail_read : {false, true}) {
        reset_storage("3");
        throw_on_load = fail_read;
        throw_on_save = !fail_read;
        CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Clean, tk::RebootReasonState::Missing));
        CHECK(tk::safe_mode_active());
    }

    // Allocation failures at either NVS boundary fail closed into safe mode and never unwind
    // through app_main's boot decision.
    reset_storage("0");
    throw_on_load = true;
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
    CHECK(tk::safe_mode_active());

    reset_storage("0");
    throw_on_save = true;
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
    CHECK(tk::safe_mode_active());

    reset_storage("0");
    throw_other_on_load = true;
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
    CHECK(tk::safe_mode_active());

    // Ordinary NVS failures do not throw, but are just as safety-relevant: neither an unreadable
    // nor an unwritable crash counter may authorize the BLE/MQTT stack on a possible reboot loop.
    reset_storage("0");
    load_ok = false;
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
    CHECK(saved_counts.empty());
    CHECK(tk::safe_mode_active());

    reset_storage("0");
    save_ok = false;
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
    CHECK(saved_counts.size() == 1 && saved_counts[0] == "1");
    CHECK(tk::safe_mode_active());

    // A present but non-canonical safety value is neither Missing nor zero. Every corruption
    // shape fails closed before a replacement write can hide the evidence.
    for (const char* malformed : {"", "abc", "3garbage", "-1", "+1", "01", "101",
                                  "999999999999999999999999"}) {
        reset_storage(malformed);
        stored_present = true;
        CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
        CHECK(saved_counts.empty());
        CHECK(tk::safe_mode_active());
    }

    // The independent supervisor now owns the qualified long-run clear. These real startup
    // tests establish its pending work; actual owner/health/time/failure cases live in
    // test_heap_supervisor.cpp, not an obsolete one-shot timer fixture.
    reset_storage("0");
    CHECK(!tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
    CHECK(tk::safe_mode_healthy_clear_pending());
    reset_storage("3");
    CHECK(tk::safe_mode_begin(storage, tk::BootCounterCause::Fault, tk::RebootReasonState::Missing));
    CHECK(!tk::safe_mode_healthy_clear_pending());

    // The heap watchdog may reboot only after the next counter is durable. Exercise the real
    // noexcept glue against normal failures and both exception classes; no failure can be
    // mistaken for authorization to call esp_restart().
    reset_storage("");
    CHECK(tk::persist_reboot_reason(&storage, "heap:1"));
    CHECK(saved_counts.size() == 1 && saved_counts[0] == "heap:1");

    reset_storage("");
    save_ok = false;
    CHECK(!tk::persist_reboot_reason(&storage, "heap:1"));

    reset_storage("");
    throw_on_save = true;
    CHECK(!tk::persist_reboot_reason(&storage, "heap:1"));

    reset_storage("");
    throw_other_on_save = true;
    CHECK(!tk::persist_reboot_reason(&storage, "heap:1"));
    CHECK(!tk::persist_reboot_reason(nullptr, "heap:1"));

    // Missing is the only ordinary boot. A present breadcrumb is consumed with erase (never an
    // empty-string write), while read/erase ambiguity returns the cap and closes the restart
    // ladder plus the post-restart activity window.
    reset_storage("");
    tk::RebootReasonRecord reason = tk::take_reboot_reason(storage);
    CHECK(reason.state == tk::RebootReasonState::Missing);
    CHECK(reason.text[0] == '\0');
    CHECK(reason.heap_restarts == 0);
    CHECK(remove_calls == 0);

    reset_storage("heap:3");
    reason = tk::take_reboot_reason(storage);
    CHECK(reason.state == tk::RebootReasonState::Present);
    CHECK(std::strcmp(reason.text, "heap:3") == 0);
    CHECK(reason.heap_restarts == 3);
    CHECK(remove_calls == 1);
    CHECK(saved_counts.empty());

    for (const char* invalid : {"garbage", "heap:0", "heap:6", "heap:01"}) {
        reset_storage(invalid);
        reason = tk::take_reboot_reason(storage);
        CHECK(reason.state == tk::RebootReasonState::Error);
        CHECK(std::strcmp(reason.text, "heap:nvs-invalid") == 0);
        CHECK(reason.heap_restarts == tk::kHeapMaxConsecutiveRestarts);
        CHECK(remove_calls == 1);
    }

    reset_storage("heap:3");
    load_ok = false;
    reason = tk::take_reboot_reason(storage);
    CHECK(reason.state == tk::RebootReasonState::Error);
    CHECK(reason.heap_restarts == tk::kHeapMaxConsecutiveRestarts);
    CHECK(remove_calls == 0);

    reset_storage("heap:3");
    remove_ok = false;
    reason = tk::take_reboot_reason(storage);
    CHECK(reason.state == tk::RebootReasonState::Error);
    CHECK(std::strcmp(reason.text, "heap:nvs-clear-error") == 0);
    CHECK(reason.heap_restarts == tk::kHeapMaxConsecutiveRestarts);
    CHECK(remove_calls == 1);

    reset_storage("heap:3");
    throw_on_remove = true;
    reason = tk::take_reboot_reason(storage);
    CHECK(reason.state == tk::RebootReasonState::Error);
    CHECK(reason.heap_restarts == tk::kHeapMaxConsecutiveRestarts);

    reset_storage("heap:3");
    throw_on_load = true;
    reason = tk::take_reboot_reason(storage);
    CHECK(reason.state == tk::RebootReasonState::Error);
    CHECK(std::strcmp(reason.text, "heap:nvs-read-error") == 0);
    CHECK(reason.heap_restarts == tk::kHeapMaxConsecutiveRestarts);

    // The fail-closed record itself must remain usable when every global C++ allocation is
    // refused. This catches a std::string (or another allocating owner) being reintroduced into
    // the noexcept NVS-error/catch path, where bad_alloc would otherwise call std::terminate.
    reset_storage("heap:3");
    load_ok = false;
    fail_global_allocation = true;
    reason = tk::take_reboot_reason(storage);
    fail_global_allocation = false;
    CHECK(reason.state == tk::RebootReasonState::Error);
    CHECK(std::strcmp(reason.text, "heap:nvs-read-error") == 0);
    CHECK(reason.heap_restarts == tk::kHeapMaxConsecutiveRestarts);

    reset_storage("heap:3");
    throw_on_load = true;
    fail_global_allocation = true;
    reason = tk::take_reboot_reason(storage);
    fail_global_allocation = false;
    CHECK(reason.state == tk::RebootReasonState::Error);
    CHECK(std::strcmp(reason.text, "heap:nvs-read-error") == 0);

    std::cout << "OK " << checks << " runtime boundary checks passed\n";
    return 0;
}
