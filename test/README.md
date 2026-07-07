# Host-side mock build (`build_mock/`)

A small, hardware-free test target that compiles and runs the project's **pure logic**
with the system toolchain — **no ESP-IDF, no Docker, no USB board**. It gives a real
"run it and see" loop in seconds, in any environment: a local terminal, CI, or a Claude
Code web session (which [cannot build firmware or flash](../.claude/CLAUDE.md#environment-note-claude-code-on-the-web--remote-sandbox)).

## Run it

```bash
scripts/run-mock-tests.sh        # configure + build + ctest, one shot
```

or manually:

```bash
cmake -S test -B build_mock      # build_mock/ is gitignored
cmake --build build_mock
ctest --test-dir build_mock --output-on-failure
```

Requires only a C++17 compiler (`g++`/`clang++`); `cmake` is used when present, and
`scripts/run-mock-tests.sh` falls back to invoking the compiler directly otherwise —
the suite is a single translation unit, so on a cmake-less host this is equivalent:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -Imain -o build_mock/logic_tests test/test_logic.cpp
./build_mock/logic_tests
```

The test binary is dependency-free (no gtest) and returns non-zero on the first
failed check.

CI runs this as the `logic-test` job, a **fast gate the per-target firmware build
depends on** (`.github/workflows/build.yml`) — a logic regression fails in seconds
instead of after four ESP-IDF builds.

## What's covered

The firmware delegates these decision/conversion cores to IDF-free headers under
[`main/logic/`](../main/logic), so the same code the device runs is what gets tested:

| Logic | Header | Firmware call sites |
|-------|--------|---------------------|
| VIN plausibility (17-char, A–Z0–9 ∖ I/O/Q) | `logic/vin.hpp` | `VehicleController::vin_is_plausible`, `/set_vin`, pairing gate |
| Imperial → metric (km, km/h, odometer) | `logic/units.hpp` | MQTT/HA bridge, drive-state telemetry |
| `link_state()` four-state machine + the debounced-ASLEEP asymmetry | `logic/link_state.hpp` | `VehicleController::link_state()` |
| `/status` `link` + MQTT `sleep_status` strings | `logic/link_state.hpp` | `http_status.cpp`, `mqtt_ha.cpp` |
| Per-target platform name + OTA image suffix | `logic/target.hpp` | `platform.hpp` (`TK_PLATFORM`), `ota_update.cpp` |
| MCP protocol core (version negotiation, JSON-RPC method routing, tool/arg-spec registry, int clamp) | `logic/mcp.hpp` | `mcp_server.cpp` (`/mcp` schema + executor) |
| Shared command-outcome text (success / Tesla reason / unreachable) | `logic/command_result.hpp` | `http_api.cpp` `/command` reason, `mcp_server.cpp` tools/call result |
| On-device display presenter (hero priority ladder, SoC gradient, RSSI→bars, SSID scroll) reading the shared UI snapshot | `logic/display_model.hpp`, `logic/ui_state.hpp` | `display.cpp` renderer (via `VehicleController::ui_snapshot()`) |

The target mapping is double-locked: `ota_update.cpp` `static_assert`s its compile-time
image-suffix literal against `tk::image_suffix()`, so the macro and the host-tested
mapping cannot drift.

## What's *not* covered (by design)

The cJSON envelope builders for `/status` and the MQTT discovery/state payloads stay
IDF/cJSON-coupled and out of this mock. What regresses silently in them is the *pure
inputs* — the unit conversions and `link_state` strings that feed those payloads — and
those are covered above. Anything touching NimBLE, NVS or `esp_http_server` stays out of
scope or behind thin seams.

## Adding to it

Put new hardware-free logic in `main/logic/` (keep it free of IDF/FreeRTOS/NimBLE/NVS/
cJSON includes so it stays host-compilable), have the firmware delegate to it, then add
`CHECK(...)` cases in `test_logic.cpp`. No new files or CMake edits needed for extra
checks in the existing suite.
