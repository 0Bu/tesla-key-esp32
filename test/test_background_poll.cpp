// Compile the actual enqueue body with the real bounded runner and passive identity/clock inputs.
#include "logic/command_runner.hpp"
#include "logic/telemetry_poll.hpp"
#include "logic/vehicle_data.hpp"
#include "rtos_guard.hpp"
#include <stdexcept>
#include <freertos/task.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

// Symbolic selector adapters test dispatch choice, not nanopb tag values or wire bytes.
enum FixtureSelector { CarServer_GetVehicleData_getClimateState_tag = 1,
    CarServer_GetVehicleData_getDriveState_tag, CarServer_GetVehicleData_getTirePressureState_tag,
    CarServer_GetVehicleData_getClosuresState_tag };
namespace TeslaBLE {
class Client {
public:
    int selector{};
    int build_car_server_get_vehicle_data_message(uint8_t*, size_t*, int selected) {
        selector = selected;
        return 0;
    }
};
}
namespace {
unsigned checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::fprintf(stderr, "background poll: %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

class BackgroundPollFixture {
public:
    using Builder = std::function<int(TeslaBLE::Client*, uint8_t*, size_t*)>;
    std::atomic<bool> cmd_in_flight_{false};
    bool identity_ready{true};
    tk::CommandRunner command_runner_;
    Builder command_builders_[tk::CommandRunner::kMaxQueueSize];
    SemaphoreHandle_t vehicle_mutex_{xSemaphoreCreateMutex()};
    bool climate_usable{true};
    bool throw_climate{false};
    bool check_locked{false};
    ~BackgroundPollFixture() { vSemaphoreDelete(vehicle_mutex_); }
    bool get_vehicle_climate(tk::VehicleClimateData&) const {
        CHECK(xSemaphoreTake(vehicle_mutex_, 0) == pdTRUE); // snapshot must precede Vehicle ownership
        CHECK(xSemaphoreGive(vehicle_mutex_) == pdTRUE);
        if (throw_climate) throw std::runtime_error("passive cache failure");
        return climate_usable;
    }
    bool command_identity_ready_() const {
        if (check_locked) CHECK(xSemaphoreTake(vehicle_mutex_, 0) == pdFALSE);
        return identity_ready;
    }
    void enqueue_telemetry_round_(tk::TelemetryPollState& telemetry_poll);
    bool enqueue_background_poll_(const std::string& name, tk::BleDomain domain, Builder builder);
};
#include "background_poll_adapter.generated.hpp"
} // namespace

static void test_actual_telemetry_round() {
    for (bool usable : {true, false}) {
        BackgroundPollFixture f;
        f.climate_usable = usable;
        f.check_locked = true;
        tk::TelemetryPollState poll{};
        TeslaBLE::Client client;
        const int missing[] = {1, 1, 2, 1, 1, 3, 1, 1, 4};
        for (unsigned turn = 0; turn < 9; ++turn) {
            f.enqueue_telemetry_round_(poll);
            CHECK(f.command_runner_.queue_size() == 1);
            const auto* cmd = f.command_runner_.current_command();
            CHECK(cmd && cmd->wake_policy == tk::WakePolicy::NoWakeSkip && cmd->effect == tk::CommandEffect::ReadOnly);
            size_t length = 0;
            CHECK(f.command_builders_[cmd->id % tk::CommandRunner::kMaxQueueSize](&client, nullptr, &length) == 0);
            CHECK(client.selector == (usable ? static_cast<int>(turn % 4 + 1) : missing[turn]));
            f.command_builders_[cmd->id % tk::CommandRunner::kMaxQueueSize] = nullptr;
            f.command_runner_.pop_current();
        }
        const auto prior = poll;
        f.cmd_in_flight_.store(true);
        f.enqueue_telemetry_round_(poll);
        CHECK(f.command_runner_.queue_size() == 0);
        CHECK(poll.rotation == prior.rotation && poll.climate_attempts == prior.climate_attempts);
        f.cmd_in_flight_.store(false);
        f.throw_climate = true;
        bool caught = false;
        try { f.enqueue_telemetry_round_(poll); } catch (const std::runtime_error&) { caught = true; }
        CHECK(caught && f.command_runner_.queue_size() == 0);
        CHECK(poll.rotation == prior.rotation && poll.climate_attempts == prior.climate_attempts);
    }
}

int test_background_poll() {
    test_actual_telemetry_round();
    BackgroundPollFixture f;
    unsigned builder_calls = 0;
    const auto builder = [&](TeslaBLE::Client*, uint8_t*, size_t*) { ++builder_calls; return 0; };
    // Deterministically stage the race: the scheduler observes idle, then a foreground owner
    // starts before this task acquires Vehicle lock. The actual under-lock seam must recheck.
    const bool scheduler_observed_idle = !f.cmd_in_flight_.load();
    CHECK(scheduler_observed_idle);
    f.cmd_in_flight_.store(true);
    CHECK(!f.enqueue_background_poll_("Charge State Poll", tk::BleDomain::Infotainment, builder));
    CHECK(f.command_runner_.queue_size() == 0);
    for (const auto& slot : f.command_builders_) CHECK(!slot);
    CHECK(builder_calls == 0);

    f.cmd_in_flight_.store(false);
    f.identity_ready = false;
    CHECK(!f.enqueue_background_poll_("Charge State Poll", tk::BleDomain::Infotainment, builder));
    CHECK(f.command_runner_.queue_size() == 0);
    f.identity_ready = true;
    CHECK(f.enqueue_background_poll_("Charge State Poll", tk::BleDomain::Infotainment, builder));
    const auto* cmd = f.command_runner_.current_command();
    CHECK(cmd != nullptr);
    CHECK(cmd->name == "Charge State Poll");
    CHECK(cmd->domain == tk::BleDomain::Infotainment);
    CHECK(cmd->wake_policy == tk::WakePolicy::NoWakeSkip);
    CHECK(cmd->effect == tk::CommandEffect::ReadOnly);
    CHECK(cmd->timeout_ms == 10000);
    CHECK(cmd->enqueued_at_ms == static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount())));
    CHECK(static_cast<bool>(f.command_builders_[cmd->id % tk::CommandRunner::kMaxQueueSize]));
    CHECK(builder_calls == 0);
    for (size_t i = 1; i < tk::CommandRunner::kMaxQueueSize; ++i)
        CHECK(f.enqueue_background_poll_("Drive State Poll", tk::BleDomain::Infotainment, builder));
    CHECK(!f.enqueue_background_poll_("Full", tk::BleDomain::Infotainment, builder));
    CHECK(f.command_runner_.queue_size() == tk::CommandRunner::kMaxQueueSize);
    CHECK(builder_calls == 0);
    std::printf("OK %u actual background enqueue checks passed\n", checks);
    return static_cast<int>(checks);
}
