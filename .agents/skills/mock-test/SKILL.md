---
name: mock-test
description: Fast host-only mock and sanitizer test runner for IDF-free logic and boundaries.
---

Invoke `$mock-test`. Read [AGENTS.md](../../../AGENTS.md) first; this workflow grants no
additional authorization. Canonical skills live under `.agents/skills/`; hooks enforce local policy.

# mock-test — fast host-side logic, mock, and sanitizer execution

Fast C++ checks use the host toolchain without ESP-IDF or hardware. The strict suite also needs
a real Linux browser: on macOS it delegates that gate to Docker, never native Chrome.

## Available Entry Points

1. **Targeted Fast Tests (`scripts/run-fast-tests.sh`)**:
   - Compiles and runs isolated test targets in seconds:
     - Pure logic only: `scripts/run-fast-tests.sh --logic`
     - NVS storage adapter: `scripts/run-fast-tests.sh --nvs`
     - Runtime boundary & log mutex: `scripts/run-fast-tests.sh --boundary`
     - Linux sanitizers: `scripts/run-fast-tests.sh --sanitizers`
     - Portable logic, NVS and native runtime families: `scripts/run-fast-tests.sh --all`
     - `--self-test` checks only compiler availability. The complete mock suite requires CMake;
       `--all` does not run Linux sanitizers, which retain their separate mandatory handoff gate.

2. **Full Host Suite (`scripts/run-mock-tests.sh`)**:
   - Runs CMake host tests, display simulator parity check, and JS protocol vectors:
     ```bash
     scripts/run-mock-tests.sh
     ```
   - CI-equivalent fail-closed suite (requires all tools, browser DOM gate):
     ```bash
     scripts/run-mock-tests.sh --require-all
     ```
   - On macOS, prepare a cold browser image with `scripts/run-browser-tests-linux.sh` first.
     Strict mode requires Docker and an actual successful Linux desktop/mobile DOM check within
     the existing 120-second outer gate. Missing Docker or browser execution fails the suite;
     controlled launch-policy tests and optional skips do not establish browser coverage.

3. **Linux Address / Undefined-Behavior / Leak Sanitizers (`scripts/run-sanitizer-tests.sh`)**:
   - On a Linux host, the default run executes every host test target, the fixed-seed fuzz corpus
     and every sanitized contract probe that the script declares, under ASan + UBSan + LSan;
     `--self-test` runs the deliberate detector tripwires instead. CI runs both:
     ```bash
     scripts/run-sanitizer-tests.sh --self-test
     scripts/run-sanitizer-tests.sh
     ```

## Local reuse and PR feedback

`scripts/check-local.sh --firmware` runs the complete local handoff: agent/repository checks, full
host, protocol, Linux sanitizers and the pinned four-target disposable-signing/reproducibility path.
Omit `--firmware` only when firmware compilation is not applicable.

Use `scripts/with-host-python.sh python3 scripts/run-local-checks.py --require-all` to share one
successful full local host run between iteration, Stop and strict pre-commit checks. `--force`
reruns it. The coordinator binds actual working-tree inputs and runtime and never certifies CI,
review, signing or hardware. Pre-commit disables reuse for a working tree differing from the index.
See [Development](../../../docs/DEVELOPMENT.md#validate-a-change) for limits and cancellation.

PR CI can use `run-mock-tests.sh --require-all --profile runtime` or `--profile browser`; docs-only
feedback uses repository lint. These selected profiles are not full handoff evidence. Unknown or
combined runtime/browser code changes, main and manual publication runs retain full host coverage, protocol harness and
sanitizers. Local handoff still runs the complete applicable contract.

## Boundaries and Guidelines

- **Read-only**: This workflow compiles and runs tests only. It does not authorize commits, pushes,
  PR creation, merges, flashing, OTA, or vehicle communication.
- **Evidence separation**: Host test success is not IDF compilation proof; report host results
  distinct from Docker, signing, hardware, and vehicle evidence.
