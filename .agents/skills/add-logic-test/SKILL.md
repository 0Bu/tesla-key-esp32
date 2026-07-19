---
name: add-logic-test
description: Scaffold a new hardware-free pure-logic unit in main/logic/ and its CHECK cases in test/test_logic.cpp, so a decision/conversion the firmware makes is verified by the host-side mock build (the local "run it and see" loop that CI gates on). Use when extracting testable logic out of vehicle_ctrl/http_server/mqtt_ha/ota, adding a CHECK to the mock suite, or when asked to "add a logic test", "make this testable", "cover this in the mock build", or "add a pure-logic header".
---

# add-logic-test — extract pure logic + cover it in the host mock build

This project's local verification loop lives in [`test/`](../../../test/): IDF-free decision and
conversion cores live in [`main/logic/`](../../../main/logic/), the firmware **delegates** to them,
and [`test/test_logic.cpp`](../../../test/test_logic.cpp) exercises them with a plain host compiler
in seconds — no ESP-IDF, no Docker, no board. CI's `logic-test` job gates the firmware build on it,
and the `run-logic-tests.sh` Stop hook gates the end of a turn on it. This skill walks the exact
steps to add a new unit without breaking those invariants.

**Single source of truth is the whole point:** the device must run the *same* code the test runs.
Never copy logic into a header and leave the original behind — make the firmware call `tk::…`.

## When NOT to use this

If the behaviour can't be separated from IDF/FreeRTOS/NimBLE/NVS/cJSON/`esp_http_server` (it's
inherently about hardware or the network), it doesn't belong in `main/logic/`. Extract only the
*pure input* — the conversion or decision — and leave the I/O seam in the `.cpp`. (See
[`test/README.md`](../../../test/README.md) "What's *not* covered".)

## Steps

### 1. Create `main/logic/<name>.hpp`

Header-only, `#pragma once`, **standard-library includes only** (e.g. `<string>`, `<cstdint>`,
`<cmath>`) — no IDF/FreeRTOS/NimBLE/NVS/cJSON. Put everything in `namespace tk`. Open with the
standard guard comment so the constraint travels with the file (copy the one at the top of
[`main/logic/vin.hpp`](../../../main/logic/vin.hpp)). Document where the firmware delegates from.

```cpp
#pragma once

#include <cstdint>

// Pure, hardware-free logic shared by the firmware and the host-side mock build
// (test/, built without ESP-IDF). Keep this file free of IDF/FreeRTOS/NimBLE/NVS/
// cJSON/esp_http_server includes so it compiles with a plain host toolchain.
// Single source of truth — <FirmwareCallSite> delegates here. See test/README.md.
namespace tk {

inline int example(int x) { return x * 2; }

}  // namespace tk
```

### 2. Make the firmware delegate

In the relevant `.cpp`/`.hpp` (e.g. `vehicle_ctrl.cpp`, `http_server.cpp`, `mqtt_ha.cpp`,
`ota_update.cpp`), `#include "logic/<name>.hpp"` and replace the inline computation with a call to
`tk::<fn>()`. Delete the old inline copy — no duplicated logic.

### 3. Add CHECK cases to `test/test_logic.cpp`

**No new files and no CMake edits** — the suite is one translation unit and the include dir
already points at `main/` ([`test/CMakeLists.txt`](../../../test/CMakeLists.txt)). Three edits in
`test_logic.cpp`:

1. Add `#include "logic/<name>.hpp"` with the other logic includes.
2. Add a `static void test_<name>() { … }` with `CHECK(...)` cases.
3. Call `test_<name>();` from `main()` alongside the existing `test_*()` calls.

Available macros: `CHECK(cond)`, `CHECK_STR(got, want)` (nullptr-safe C-string compare),
`CHECK_NEAR(got, want)` (float tolerance `1e-6`). Cover the way the existing tests do — not just
the happy path but **boundaries, asymmetries and negatives** (study `test_vin`, `test_units`,
`test_link_state`): off-by-one at each threshold, rejected inputs, the "must-NOT-happen" cases.

```cpp
static void test_example() {
    CHECK(tk::example(0) == 0);
    CHECK(tk::example(21) == 42);
    CHECK(tk::example(-1) == -2);   // negative path, not just the happy one
}
// …and add `test_example();` inside main().
```

### 4. Run the mock build — it must print `OK`

```bash
scripts/run-mock-tests.sh
```

(Equivalent to `cmake -S test -B build_mock && cmake --build build_mock && ctest --test-dir
build_mock --output-on-failure`; on a host without cmake the script falls back to compiling
`test/test_logic.cpp` directly with g++/clang++ — same flags, same result.) It compiles with
`-Wall -Wextra -Werror`, so warnings fail too.
A red result blocks the Stop hook — fix it before claiming done.

### 5. (If the value mirrors a compile-time constant) lock it with a `static_assert`

When the firmware also hard-codes the value as a macro/literal, mirror the `target.hpp` pattern:
`static_assert` the compile-time literal against `tk::<fn>(...)` at its call site so the macro and
the host-tested mapping can't drift (see the image-suffix `static_assert` in
[`main/ota_update.cpp`](../../../main/ota_update.cpp), described in
[`test/README.md`](../../../test/README.md)).

### 6. Keep docs in sync

If the new unit is a notable core, add a row to the coverage table in
[`test/README.md`](../../../test/README.md). If it changes how a documented subsystem behaves,
the `project-review` skill will flag any doc/code drift — run it before merging.
