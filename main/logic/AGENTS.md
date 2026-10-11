# Pure logic (`main/logic/`)

Apply [repository policy](../../AGENTS.md).

- Use standard C++17 only; no ESP-IDF, FreeRTOS, NimBLE, NVS or cJSON dependency.
- Every header names an explicit host-test owner in `test/logic_test_ownership.json`.
- Add or adjust `CHECK` coverage in `test/test_logic.cpp` for changed decisions and transitions;
  the firmware must delegate to the tested unit. Mirrored presenters also need parity evidence.
- Run `scripts/run-mock-tests.sh` before committing; follow the
  [test guide](../../test/README.md#adding-to-it) for ownership and integration limits.
