# Pure Logic & Host Testability Contract (`main/logic/`)

This scoped policy governs hardware-independent pure-logic code in `main/logic/`.
It complements the canonical repository policy in [`../../AGENTS.md`](../../AGENTS.md).

## 1. Hardware & IDF Independence

- All headers and translation units in `main/logic/` must be completely free of ESP-IDF dependencies and hardware-specific abstractions.
- Use only standard C++17 library components.
- Do NOT include any ESP-IDF headers (`esp_...`), FreeRTOS headers (`freertos/...`), or NVS headers (`nvs.h`, `nvs_flash.h`).

## 2. Test-First Mandatory Coverage

- Every pure-logic algorithm, state transition, conversion, or formatting decision in `main/logic/` must have an explicit host-test owner in `test/logic_test_ownership.json`.
- Any addition or modification must be accompanied by corresponding unit tests (`CHECK` assertions) in `test/test_logic.cpp`.
- The local host test suite (`scripts/run-mock-tests.sh`) is the primary development feedback loop. Ensure all host tests pass before committing.
