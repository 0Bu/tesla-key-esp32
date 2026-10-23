#!/usr/bin/env python3
"""Compile the actual cache snapshot and indicator input against passive RTOS facades."""

from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

from test_runtime_boundary_contract import function_body_in, scrub_cpp_preserving_layout, scope_around


ROOT = Path(__file__).resolve().parents[1]


def require_http_snapshot(source: str) -> None:
    body = scrub_cpp_preserving_layout(function_body_in(source, "build_status_object"))
    code = re.sub(r"\s+", "", body)
    statement = "in.charge = g_vehicle->get_cached_charge_with_age(in.have_charge_age, in.charge_age_s);"
    assert code.count("in.vcsec_sleep=g_vehicle->vcsec_sleep_raw();" + re.sub(r"\s+", "", statement)) == 1
    assert scope_around(body, statement)[0] == 0, "charge snapshot must be unconditional"
    assert not re.search(r"(?m)^\s*#", body), "status producer must not preprocess away the snapshot"
    # No later hardcoded age or second cache read may replace the coherent result.
    assert re.findall(r"\bin\.(charge|have_charge_age|charge_age_s)\b", body) == [
        "charge", "have_charge_age", "charge_age_s"]


def main() -> None:
    source = (ROOT / "main/vehicle_ctrl.cpp").read_text()
    getter = function_body_in(source, "get_cached_charge_with_age")
    indicator = function_body_in(source, "ui_snapshot")
    layout = scrub_cpp_preserving_layout(getter)
    start, end = scope_around(layout, "tk::SemGuard g(cache_mutex_);")
    for statement in ("charge = last_known_charge_;", "sample = last_charge_ticks_.load();",
                      "have_age = charge_state_generation_.load() != 0;"):
        assert start < layout.index(statement) < end, statement
    assert layout.index("xTaskGetTickCount()") > end
    producer = (ROOT / "main/http_status.cpp").read_text()
    require_http_snapshot(producer)
    expected = "in.charge = g_vehicle->get_cached_charge_with_age(in.have_charge_age, in.charge_age_s);"
    assert producer.count(expected) == 1
    for replacement in (
        "// " + expected + "\n    in.charge = g_vehicle->get_cached_charge(); in.have_charge_age = true; in.charge_age_s = 0;",
        expected + "\n    in.have_charge_age = true; in.charge_age_s = 0;",
        "if (false) { " + expected + " }",
        "#if 0\n    " + expected + "\n#endif",
    ):
        try:
            require_http_snapshot(producer.replace(expected, replacement, 1))
        except AssertionError:
            pass
        else:
            raise AssertionError("HTTP comment-decoy/hardcoded-age control escaped")
    require_http_snapshot(producer)

    facade = r'''
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include "logic/charge_control.hpp"
#include "logic/vehicle_data.hpp"
#include "logic/ui_state.hpp"
static uint32_t ticks = 0;
static constexpr uint32_t configTICK_RATE_HZ = 100;
static uint32_t xTaskGetTickCount() { return ticks; }
struct Mutex { bool available = true, held = false; std::function<void()> after_unlock; };
namespace tk {
struct SemGuard {
    Mutex* mutex;
    explicit SemGuard(Mutex* m) : mutex(m && m->available ? m : nullptr) {
        if (mutex) { assert(!mutex->held); mutex->held = true; }
    }
    explicit operator bool() const { return mutex != nullptr; }
    ~SemGuard() {
        if (mutex) {
            mutex->held = false;
            auto after = std::move(mutex->after_unlock);
            mutex->after_unlock = {};
            if (after) after();
        }
    }
};
}
struct VehicleController {
    Mutex mutex;
    Mutex* cache_mutex_ = &mutex;
    ChargeStateResult last_known_charge_;
    std::atomic<uint32_t> last_charge_ticks_{0}, charge_state_generation_{0};
    tk::LinkState link = tk::LinkState::Awake;
    ChargeStateResult get_cached_charge_with_age(bool& have_age, uint32_t& age_s);
    tk::UiSnapshot ui_snapshot();
    tk::LinkState link_state() const { return link; }
    bool ble_connected() const { return true; }
    bool ble_rssi(int8_t& rssi) const { rssi = -60; return true; }
};
ChargeStateResult VehicleController::get_cached_charge_with_age(bool& have_age, uint32_t& age_s)
''' + getter + r'''
tk::UiSnapshot VehicleController::ui_snapshot()
''' + indicator + r'''
int main() {
    VehicleController vc;
    vc.last_known_charge_.valid = true;
    vc.last_known_charge_.has_battery_level = true;
    vc.last_known_charge_.battery_level = 0;
    vc.last_known_charge_.charging_state = "Charging";
    vc.charge_state_generation_ = 1;
    for (uint32_t age : {0u, 29u, 30u, 31u, 86400u}) {
        ticks = age * configTICK_RATE_HZ;
        for (auto link : {tk::LinkState::Awake, tk::LinkState::Asleep, tk::LinkState::Idle,
                          tk::LinkState::Unknown, tk::LinkState::Unreachable}) {
            vc.link = link;
            bool have = false; uint32_t reported = 999;
            auto cs = vc.get_cached_charge_with_age(have, reported);
            assert(cs.valid && have && reported == age && !vc.mutex.held);
            auto ui = vc.ui_snapshot();
            assert(ui.have_soc && ui.soc == 0);
            assert(ui.charging == (link == tk::LinkState::Awake && age <= 30));
        }
    }
    vc.link = tk::LinkState::Awake;
    ticks = 0; vc.charge_state_generation_ = 0;
    assert(!vc.ui_snapshot().charging);
    vc.charge_state_generation_ = 1;
    assert(vc.ui_snapshot().charging); // legitimate tick-zero sample
    vc.last_known_charge_.valid = false;
    assert(!vc.ui_snapshot().charging);
    vc.last_known_charge_.valid = true;
    vc.last_charge_ticks_ = UINT32_MAX - 1999; ticks = 1000;
    bool have = false; uint32_t age = 999;
    vc.get_cached_charge_with_age(have, age);
    assert(have && age == 30); // monotonic tick wrap
    vc.last_charge_ticks_ = 0; ticks = 4000;
    vc.mutex.after_unlock = [&] {
        vc.last_known_charge_.battery_level = 80;
        vc.last_charge_ticks_ = ticks;
    };
    auto old = vc.get_cached_charge_with_age(have, age);
    assert(old.battery_level == 0 && have && age == 40); // no old value/new stamp mixture
    auto fresh = vc.get_cached_charge_with_age(have, age);
    assert(fresh.battery_level == 80 && age == 0);
    vc.mutex.available = false;
    have = true; age = 999;
    assert(!vc.get_cached_charge_with_age(have, age).valid && !have && age == 0);
    assert(!vc.ui_snapshot().charging && !vc.mutex.held);
}
'''
    compiler = shutil.which(os.environ.get("CXX", "c++"))
    assert compiler, "host C++ compiler is required"
    with tempfile.TemporaryDirectory(prefix="charge-presentation-") as directory:
        work = Path(directory)
        cpp, binary = work / "probe.cpp", work / "probe"
        cpp.write_text(facade)
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT/"main"),
                        str(cpp), "-o", str(binary)], check=True, timeout=40)
        subprocess.run([str(binary)], check=True, timeout=10)
    print("charge presentation: PASS (actual coherent snapshot/indicator, tick zero/wrap, lock failure and post-unlock publication)")


if __name__ == "__main__":
    main()
