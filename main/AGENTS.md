# Embedded C++ & FreeRTOS Heap Safety Contract (`main/`)

This scoped policy governs firmware implementation and maintenance in `main/`.
It complements the canonical repository policy in [`../AGENTS.md`](../AGENTS.md).

## 1. Heap & Allocation Discipline

- On ESP32 the critical limiting heap measure is the **largest contiguous free block**, not total free heap.
- Avoid repeated dynamic allocations (`malloc`, `new`, temporary `std::string`, `std::vector`) in cyclic tasks, event loops, BLE callbacks, or HTTP handlers.
- Stream HTTP and JSON responses in bounded chunks; never buffer large monolithic payloads in RAM.
- When using cJSON: Every error and return path must explicitly free allocated JSON trees with `cJSON_Delete()` to prevent heap leaks.
- Prefer bounded transformations in `main/logic/`; keep IDF handles, tasks, and mutable state in the integration shell.

## 2. FreeRTOS Task Stacks

- FreeRTOS tasks have small, fixed stack allocations (typically 3–4 KiB).
- Never place large buffers, arrays, or bulky C++ objects as local variables on the task stack.
- Use static allocations or dedicated bounded pools for large buffers.
- Keep task stack usage verifiable and within the reviewed baseline limits (`scripts/firmware-stack-baseline.json`).

## 3. Mutex & Concurrency Hierarchy

- Never allocate memory, perform network or flash I/O, emit log lines (`ESP_LOGx`), or call throwing code while holding a shared mutex.
- Strictly adhere to the documented lock pattern: snapshot state under lock, release the mutex immediately, and execute operations on the local copy.
- Follow the documented ownership model in [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).

## 4. Exception & OOM Containment

- Every HTTP and MCP request path must remain inside the server's exception and OOM containment net (`handle_all`).
- Handle C-library allocation failures (such as cJSON returning null) explicitly because they do not throw C++ exceptions.
- Never let an uncaught exception escape a task boundary or convert malformed input into reboot loops.
