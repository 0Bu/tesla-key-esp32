---
name: mock-test
description: Fast host-only mock and sanitizer test runner for IDF-free logic and boundaries.
---

Invoke `$mock-test`. Read [AGENTS.md](../../../AGENTS.md) first; this workflow grants no
additional authorization. Canonical skills live under `.agents/skills/`; hooks enforce local policy.

# mock-test — fast host-side logic, mock, and sanitizer execution

The host-side mock build allows developers and agents to verify changes in seconds without
Docker, ESP-IDF, or hardware.

## Available Entry Points

1. **Targeted Fast Tests (`scripts/run-fast-tests.sh`)**:
   - Compiles and runs isolated test targets in seconds:
     - Pure logic only: `scripts/run-fast-tests.sh --logic`
     - NVS storage adapter: `scripts/run-fast-tests.sh --nvs`
     - Runtime boundary & log mutex: `scripts/run-fast-tests.sh --boundary`
     - Linux sanitizers: `scripts/run-fast-tests.sh --sanitizers`
     - All host units: `scripts/run-fast-tests.sh --all`

2. **Full Host Suite (`scripts/run-mock-tests.sh`)**:
   - Runs CMake host tests, display simulator parity check, and JS protocol vectors:
     ```bash
     scripts/run-mock-tests.sh
     ```
   - CI-equivalent fail-closed suite (requires all tools, browser DOM gate):
     ```bash
     scripts/run-mock-tests.sh --require-all
     ```

3. **Linux Address / Undefined-Behavior / Leak Sanitizers (`scripts/run-sanitizer-tests.sh`)**:
   - On a Linux host, the default run executes the logic, NVS and runtime-boundary host tests and
     the fixed-seed fuzz corpus under ASan + UBSan + LSan; `--self-test` runs the deliberate
     detector tripwires instead. CI runs both:
     ```bash
     scripts/run-sanitizer-tests.sh --self-test
     scripts/run-sanitizer-tests.sh
     ```

## Boundaries and Guidelines

- **Read-only**: This workflow compiles and runs tests only. It does not authorize commits, pushes,
  PR creation, merges, flashing, OTA, or vehicle communication.
- **Evidence separation**: Host test success is not IDF compilation proof; report host results
  distinct from Docker, signing, hardware, and vehicle evidence.
