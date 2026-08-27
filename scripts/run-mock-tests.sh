#!/usr/bin/env bash
# Host-side mock build: compile and run the IDF-free pure-logic tests (test/) with the
# system toolchain — no ESP-IDF, no Docker, no board. Catches logic regressions in
# seconds, in any environment (local terminal, CI, remote coding session). This is the
# real "run it and see" loop a cloud session has (it cannot build firmware or USB-flash).
#
# Usage: scripts/run-mock-tests.sh [--require-all]
# Requires: a C++17 host compiler (g++/clang++); cmake is used when present, with a
# direct-compiler fallback otherwise (see test/CMakeLists.txt, whose targets and flags the
# fallback mirrors). See test/README.md.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR=build_mock   # matches .gitignore (/build_mock/)
REQUIRE_ALL=0
if [ "${1:-}" = --require-all ]; then
    REQUIRE_ALL=1
    shift
fi
if [ "$#" -ne 0 ]; then
    echo "usage: scripts/run-mock-tests.sh [--require-all]" >&2
    exit 2
fi

TIMEOUT_RUNNER=tools/agent-hooks/run_with_timeout.py

gate_file() {
    local path="$1"
    if [ ! -f "$path" ] || [ ! -r "$path" ] || [ -L "$path" ]; then
        echo "run-mock-tests: required gate is missing, unreadable, or a symlink: $path" >&2
        exit 2
    fi
}

need_tool() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "run-mock-tests: --require-all needs $1" >&2
        exit 2
    }
}

run_gate() {
    local label="$1" seconds="$2"
    shift 2
    if [ "$REQUIRE_ALL" = 1 ]; then
        if python3 "$TIMEOUT_RUNNER" "$seconds" "$@"; then
            return 0
        else
            local rc=$?
            echo "run-mock-tests: required gate failed or timed out: $label (rc=$rc)" >&2
            exit 1
        fi
    else
        "$@"
    fi
}

if [ "$REQUIRE_ALL" = 1 ]; then
    need_tool python3
    need_tool node
    need_tool cmake
    gate_file "$TIMEOUT_RUNNER"
    for required in \
        scripts/check-display-sim-parity.sh \
        test/test_provision.py \
        test/test_capture_wake.py \
        test/test_log_origin_contract.py \
        test/test_runtime_boundary_contract.py \
        scripts/test-build-contracts.sh \
        scripts/check-dependency-contract.py \
        scripts/check-otadata-contract.py \
        scripts/apply-tesla-ble-patches.sh \
        scripts/reconcile-pr-previews.sh \
        scripts/verify-vendored-esptool-js.sh \
        scripts/check-bench-acceptance.py \
        scripts/check-release-assets.py \
        scripts/prepare-reused-release.py \
        scripts/check-published-release.py \
        scripts/check-host-gate-contract.py \
        scripts/check-logic-test-ownership.py \
        scripts/check-nvs-contract.py \
        test/logic_test_ownership.json \
        scripts/run-fuzz-smoke.sh \
        test/serial_port_release.test.mjs \
        test/web_installer.test.mjs \
        test/web_ui_http.test.mjs \
        test/tesla_protocol_vectors.test.mjs \
        scripts/check-ble-row-parity.sh \
        test/web_ui_browser_gate.py; do
        gate_file "$required"
    done
fi

if command -v cmake >/dev/null 2>&1; then
    run_gate "configure host C++ tests" 120 cmake -S test -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
    run_gate "build host C++ tests" 120 cmake --build "$BUILD_DIR" --parallel
    run_gate "execute host C++ tests" 120 ctest --test-dir "$BUILD_DIR" --output-on-failure
else
    # No cmake on this host (e.g. a bare Raspberry Pi) — compile both host targets directly.
    # Keep flags, sources and include paths in sync with test/CMakeLists.txt.
    CXX="${CXX:-}"
    if [ -z "$CXX" ]; then
        if command -v g++ >/dev/null 2>&1; then CXX=g++
        elif command -v clang++ >/dev/null 2>&1; then CXX=clang++
        else echo "run-mock-tests: need cmake or a C++17 compiler (g++/clang++)" >&2; exit 1
        fi
    fi
    mkdir -p "$BUILD_DIR"
    run_gate "compile pure-logic tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Imain -o "$BUILD_DIR/logic_tests" test/test_logic.cpp
    run_gate "execute pure-logic tests" 120 "$BUILD_DIR/logic_tests"
    run_gate "compile NVS adapter tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Itest/stubs -Imain \
        -o "$BUILD_DIR/nvs_storage_tests" \
        test/test_nvs_storage.cpp main/nvs_storage.cpp main/config_blob.cpp
    run_gate "execute NVS adapter tests" 120 "$BUILD_DIR/nvs_storage_tests"
    run_gate "compile runtime boundary tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Itest/stubs -Imain \
        -o "$BUILD_DIR/runtime_boundary_tests" \
        test/test_runtime_boundaries.cpp main/diag_log.cpp main/safe_mode.cpp
    run_gate "execute runtime boundary tests" 120 "$BUILD_DIR/runtime_boundary_tests"
fi

# Display sim ↔ C++ presenter parity: confirm tools/display_sim.py's decide() still matches
# the firmware's tk::display::compose() (so the pixel sim can't silently drift from display.cpp).
# Skipped only where python3 is unavailable — the C++ logic tests above are the hard gate.
if command -v python3 >/dev/null 2>&1; then
    run_gate "display simulator parity" 120 scripts/check-display-sim-parity.sh
    run_gate "provisioning tests" 120 python3 test/test_provision.py
    run_gate "capture wake tests" 120 python3 test/test_capture_wake.py
    run_gate "log origin contract" 120 python3 test/test_log_origin_contract.py
    run_gate "runtime C-boundary inventory" 120 python3 test/test_runtime_boundary_contract.py
    run_gate "build contract tests" 120 scripts/test-build-contracts.sh
    run_gate "exact pinned dependency contract" 120 \
        python3 scripts/check-dependency-contract.py --self-test
    run_gate "erased initial otadata contract" 120 \
        python3 scripts/check-otadata-contract.py --self-test
    run_gate "tesla-ble patch applicability" 120 scripts/apply-tesla-ble-patches.sh --self-test
    run_gate "preview reconciliation contract" 120 scripts/reconcile-pr-previews.sh --self-test
    run_gate "vendored esptool-js contract" 120 scripts/verify-vendored-esptool-js.sh
    run_gate "bench acceptance validator contract" 120 \
        python3 scripts/check-bench-acceptance.py --self-test
    run_gate "published Release asset contract" 120 \
        python3 scripts/check-release-assets.py --self-test
    run_gate "immutable Release reuse staging contract" 120 \
        python3 scripts/prepare-reused-release.py --self-test
    run_gate "deployed Release and Pages byte contract" 120 \
        python3 scripts/check-published-release.py --self-test
    run_gate "pure-logic test ownership and standalone headers" 120 \
        python3 scripts/check-logic-test-ownership.py --compile --self-test
    run_gate "host-gate execution and fallback wiring contract" 120 \
        python3 scripts/check-host-gate-contract.py
    run_gate "exact NVS namespace/key/owner/retention contract" 120 \
        python3 scripts/check-nvs-contract.py --self-test
    run_gate "deterministic parser fuzz smoke" 120 scripts/run-fuzz-smoke.sh
else
    echo "run-mock-tests: python3 not found — skipping display-sim parity check" >&2
fi

# Browser checks: exercise the inline Web Serial installer and port-release flow, then confirm the
# BLE_ROW region of main/www/app.js still decides the Bluetooth row exactly as tk::ble::decide()
# does (so the browser can't silently drift from the host-tested rules). Skipped only where node is
# unavailable — the C++ logic tests are the hard gate; CI's ubuntu-latest runner ships node.
if command -v node >/dev/null 2>&1; then
    run_gate "Node web UI contracts" 120 node --test \
        test/serial_port_release.test.mjs test/web_installer.test.mjs test/web_ui_http.test.mjs \
        test/tesla_protocol_vectors.test.mjs
    run_gate "BLE browser-row parity" 120 scripts/check-ble-row-parity.sh
    if [ "$REQUIRE_ALL" = 1 ]; then
        run_gate "real-browser DOM and accessibility contract" 120 \
            python3 test/web_ui_browser_gate.py --require-browser
    else
        python3 test/web_ui_browser_gate.py
    fi
else
    echo "run-mock-tests: node not found — skipping web-installer and BLE-row checks" >&2
fi
