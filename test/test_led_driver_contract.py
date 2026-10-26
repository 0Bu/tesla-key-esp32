#!/usr/bin/env python3
"""Run actual LED task/GPIO, OTA-state and pipeline seams with passive doubles."""
from pathlib import Path
import argparse
import os
import re
import runpy
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def pipeline_probe() -> None:
    source = (ROOT / "scripts/ci-build-all.sh").read_text()
    begin, end = '# EXACT_FOUR_TARGETS_END budget\n', '\nif [[ -z "$target_override" ]]; then\n'
    assert source.count(begin) == 1
    tail = source.split(begin, 1)[1]
    assert tail.count(end) == 1
    section = tail.split(end, 1)[0]
    with tempfile.TemporaryDirectory(prefix="led-pipeline-") as directory:
        for target in ("", "esp32s3", "esp32", "esp32c3", "esp32c6"):
            calls = Path(directory) / (target or "all")
            shell = ('set -euo pipefail; target_override="$1"; version=1.2.3; calls="$2"; '
                     'bash() { printf "%s\\n" "$*" >> "$calls"; };\n') + section
            subprocess.run(["bash", "-c", shell, "led-pipeline", target, str(calls)],
                           check=True, timeout=5)
            got = calls.read_text().splitlines() if calls.exists() else []
            expected = ["scripts/ci-build-led-variant.sh 1.2.3 dist/esp32s3/sdkconfig.esp32s3"] if target in ("", "esp32s3") else []
            assert got == expected, ("actual LED pipeline branch", target, got)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    source = (ROOT / "main/led_status.cpp").read_text()
    contracts = runpy.run_path(str(ROOT / "test/test_runtime_boundary_contract.py"))
    body_in = contracts["function_body_in"]
    parts = []
    for name in ("apa_byte", "apa_show", "led_sample", "led_task"):
        match = re.search(r"^(?:inline |\[\[gnu::noinline\]\] )?void " + name + r"\(", source, re.M)
        assert match is not None, name
        body = body_in(source, name)
        parts.append(source[match.start():source.index(body, match.start())] + body)
    # Compile the actual sampler as well as its caller; task-frame output checks their
    # execution, so an unreachable delegation cannot satisfy these source pins alone.
    task = " ".join(contracts["scrub_cpp"](body_in(source, "led_task")).split())
    for expression in ("tk::LedLatches latches{}",
                       "led_sample(v, static_cast<uint64_t>(now), snap, alerts, latches);",
                       "tk::led_frame_rgb(p, snap.soc, t)", "apa_show(color.r, color.g, color.b)"):
        assert expression in task, "missing production LED decision seam: " + expression
    sample = " ".join(contracts["scrub_cpp"](body_in(source, "led_sample")).split())
    for expression in ("alerts = tk::led_sample_alerts(latches, now, safe_mode ? false : v.reauth_required(), ota == OtaState::Error, ota == OtaState::Downloading, safe_mode ? 0 : v.ble_connect_fail(), safe_mode);",
                       "v.reauth_required()", "ota == OtaState::Error",
                       "ota == OtaState::Downloading", "v.ble_connect_fail()",
                       "if (snap.net == tk::NetLink::Wifi && !tk::net_wifi_signal(&snap.wifi_rssi, snap.ssid)) snap.net = tk::NetLink::None;"):
        assert expression in sample, "missing production LED decision seam: " + expression
    ota_source = (ROOT / "main/ota_update.cpp").read_text()
    ota_body = body_in(ota_source, "ota_get_state")
    assert re.search(r"^OtaState ota_get_state\(\) \{", ota_source, re.M)
    ota_function = "OtaState ota_get_state() " + ota_body + "\n"
    enum = re.search(r"^enum class OtaState \{[^}]*\};", (ROOT / "main/ota_update.hpp").read_text(), re.M)
    assert enum is not None
    declarations = re.findall(r"^constexpr .*(?:k(?:DI|CI|GlobalBright|FrameMs|SampleEveryFrames)|TAG)(?:\[\])?\s*=.*$", source, re.M)
    assert len(declarations) == 6
    code = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>
#include "logic/led_driver.hpp"
#include "runtime_fault_log.hpp"
#include "stack_watch.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
using gpio_num_t = int;
#define CONFIG_TESLA_LED_DI_GPIO 40
#define CONFIG_TESLA_LED_CI_GPIO 39
#define CONFIG_TESLA_LED_BRIGHTNESS 8
#define GPIO_MODE_OUTPUT 1
template<typename... T> static void log_stub(T...) {}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
struct gpio_config_t { uint64_t pin_bit_mask = 0; int mode = 0; };
static int gpio_config(gpio_config_t*) { return 0; }
struct TaskEnd {};
extern "C" void vTaskDelay(TickType_t) { throw TaskEnd{}; }
extern "C" void vTaskDelete(TaskHandle_t) { throw TaskEnd{}; }
static unsigned hwm_reads = 0;
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    assert(task == nullptr);
    ++hwm_reads;
    return 4096;
}
static int64_t task_elapsed_us = 0;
static unsigned timer_reads = 0;
static int64_t esp_timer_get_time() { return timer_reads++ == 0 ? 0 : task_elapsed_us; }
struct VehicleController {
    tk::UiSnapshot snapshot{};
    bool reauth = false;
    bool cache_forbidden = false;
    uint32_t failures = 0;
    bool reauth_required() { assert(!cache_forbidden); return reauth; }
    uint32_t ble_connect_fail() { assert(!cache_forbidden); return failures; }
    tk::UiSnapshot ui_snapshot() { assert(!cache_forbidden); return snapshot; }
    bool has_session() { assert(!cache_forbidden); return true; }
    bool session_available() { assert(!cache_forbidden); return false; }
};
static bool signal_ok = true, recovery = false;
static unsigned recovery_reads = 0;
namespace tk {
bool safe_mode_active() { ++recovery_reads; return recovery; }
NetLink net_kind() { return NetLink::Wifi; }
bool net_wifi_signal(int*, char*) { return signal_ok; }
}
static std::vector<std::pair<int, int>> events;
static int gpio_set_level(int pin, int level) {
    events.emplace_back(pin, level);
    return 0;
}
''' + enum.group(0) + r'''
static bool status_lock_available, status_guard_available, status_held;
static unsigned state_reads, ensure_calls;
struct CheckedState {
    OtaState value = OtaState::Idle;
    operator OtaState() const { assert(status_held); ++state_reads; return value; }
};
struct { CheckedState state; } s_status;
using SemaphoreHandle_t = void*;
static SemaphoreHandle_t ensure_lock() {
    ++ensure_calls;
    return status_lock_available ? reinterpret_cast<void*>(1) : nullptr;
}
namespace tk {
struct SemGuard {
    bool held;
    explicit SemGuard(SemaphoreHandle_t lock) : held(lock && status_guard_available) {
        assert(!status_held); status_held = held;
    }
    explicit operator bool() const { return held; }
    ~SemGuard() { if (held) { assert(status_held); status_held = false; } }
};
}
''' + ota_function + "\n".join(declarations + parts) + r'''
static void task_frame(VehicleController vehicle, bool wifi, OtaState ota,
                       const std::array<uint8_t, 3>& expected, int64_t elapsed_us = 0) {
    const unsigned hwm_before = hwm_reads;
    events.clear(); signal_ok = wifi;
    timer_reads = 0; task_elapsed_us = elapsed_us;
    recovery_reads = state_reads = ensure_calls = 0;
    status_lock_available = status_guard_available = true;
    s_status.state.value = ota;
    // Stop only through passive RTOS doubles: normal delay throws into the real inner catch,
    // recovery delay throws into the real outer catch, task deletion ends this test invocation.
    try { led_task(&vehicle); } catch (const TaskEnd&) {}
    // Real entry/terminal/iteration hooks delegate through the linked sampler.
    assert(hwm_reads - hwm_before >= 2);
    const auto led_minimum = tk::stack_watch_min_free_bytes(tk::StackWatch::Led);
    assert(led_minimum.has_value() && *led_minimum == 4096);
    assert(recovery_reads == 1 && state_reads == unsigned(!recovery) && ensure_calls == unsigned(!recovery));
    assert(events.size() == 2 + 12 * 8 * 3);
    assert(events[0] == std::make_pair(39, 0) && events[1] == std::make_pair(40, 0));
    std::array<uint8_t, 12> bytes{};
    size_t position = 2;
    for (auto& byte : bytes) for (int bit = 7; bit >= 0; --bit) {
        const auto event = events[position++];
        assert(event.first == 40 && (event.second == 0 || event.second == 1));
        byte |= event.second << bit;
        assert(events[position++] == std::make_pair(39, 1));
        assert(events[position++] == std::make_pair(39, 0));
    }
    assert(bytes[7] == expected[0] && bytes[6] == expected[1] && bytes[5] == expected[2]);
}
int main() {
    assert(!tk::stack_watch_min_free_bytes(tk::StackWatch::Led).has_value());
    for (bool lock : {false, true}) for (bool guard : {false, true}) {
        for (const auto state : {OtaState::Idle, OtaState::Checking, OtaState::Downloading,
                                 OtaState::Done, OtaState::Error}) {
            status_lock_available = lock; status_guard_available = guard;
            s_status.state.value = state; ensure_calls = state_reads = 0;
            const auto actual = ota_get_state();
            assert(actual == (lock && guard ? state : OtaState::Error));
            assert(!status_held && ensure_calls == 1 && state_reads == unsigned(lock && guard));
        }
    }
    for (int value = 0; value < 256; ++value) {
        events.clear();
        const auto r = static_cast<uint8_t>(value);
        const auto g = static_cast<uint8_t>(255 - value);
        const auto b = static_cast<uint8_t>(value ^ 0xa5);
        apa_show(r, g, b);
        const std::array<uint8_t, 12> expected{0, 0, 0, 0, 0xe8, b, g, r,
                                               0xff, 0xff, 0xff, 0xff};
        assert(events.size() == 12 * 8 * 3);
        size_t position = 0;
        for (const auto byte : expected) {
            for (int bit = 7; bit >= 0; --bit) {
                assert(events[position++] == std::make_pair(40, (byte >> bit) & 1));
                assert(events[position++] == std::make_pair(39, 1));
                assert(events[position++] == std::make_pair(39, 0));
            }
        }
    }
    VehicleController vehicle;
    vehicle.snapshot.have_soc = true; vehicle.snapshot.soc = 55;
    vehicle.snapshot.link_state = tk::LinkState::Awake; vehicle.snapshot.charging = true;
    task_frame(vehicle, true, OtaState::Idle, {0, 126, 23});
    task_frame(vehicle, false, OtaState::Idle, {0, 23, 146});
    vehicle.reauth = true; task_frame(vehicle, true, OtaState::Idle, {255, 0, 0});
    vehicle.reauth = false; task_frame(vehicle, true, OtaState::Error, {255, 0, 0});
    vehicle.failures = 3; task_frame(vehicle, true, OtaState::Idle, {255, 110, 0});
    vehicle.failures = 0; task_frame(vehicle, true, OtaState::Downloading, {0, 3, 20});
    vehicle.snapshot.link_state = tk::LinkState::Asleep;
    task_frame(vehicle, true, OtaState::Idle, {0, 0, 0});
    recovery = true; vehicle.cache_forbidden = true;
    for (const auto state : {OtaState::Idle, OtaState::Downloading, OtaState::Error}) {
        task_frame(vehicle, false, state, {255, 0, 0});
        task_frame(vehicle, false, state, {0, 0, 0}, 500000);
        task_frame(vehicle, false, state, {255, 0, 0}, 1000000);
    }

}
'''
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    assert compiler, "a C++17 compiler is required"
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        flags += ["-O1", "-g", "-fno-omit-frame-pointer", "-fno-sanitize-recover=all",
                  "-fsanitize=address,undefined,leak"]
    with tempfile.TemporaryDirectory(prefix="led-driver-native-") as directory:
        work = Path(directory)
        cpp, binary = work / "actual_led.cpp", work / "actual_led"
        cpp.write_text(code)
        subprocess.run([compiler, *flags, "-I" + str(ROOT / "test/stubs"),
                        "-I" + str(ROOT / "main"), str(cpp), str(ROOT / "main/stack_watch.cpp"),
                        "-o", str(binary)],
                       check=True, timeout=45)
        subprocess.run([str(binary)], check=True, timeout=10)
    pipeline_probe()
    print("LED driver: PASS (256 GPIO frames, 20 OTA-state cases, 16 actual task frames, 5 actual pipeline branches)")


if __name__ == "__main__":
    main()
