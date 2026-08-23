#!/usr/bin/env bash
# Host-side mock build: compile and run the IDF-free pure-logic tests (test/) with the
# system toolchain — no ESP-IDF, no Docker, no board. Catches logic regressions in
# seconds, in any environment (local terminal, CI, Claude Code web session). This is the
# real "run it and see" loop a cloud session has (it cannot build firmware or USB-flash).
#
# Usage: scripts/run-mock-tests.sh
# Requires: a C++17 host compiler (g++/clang++); cmake is used when present, with a
# direct-compiler fallback otherwise (see test/CMakeLists.txt, whose targets and flags the
# fallback mirrors). See test/README.md.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR=build_mock   # matches .gitignore (/build_mock/)

if command -v cmake >/dev/null 2>&1; then
    cmake -S test -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug >/dev/null
    cmake --build "$BUILD_DIR" --parallel
    ctest --test-dir "$BUILD_DIR" --output-on-failure
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
    "$CXX" -std=c++17 -Wall -Wextra -Werror -Imain -o "$BUILD_DIR/logic_tests" test/test_logic.cpp
    "$BUILD_DIR/logic_tests"
    "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Itest/stubs -Imain \
        -o "$BUILD_DIR/nvs_storage_tests" \
        test/test_nvs_storage.cpp main/nvs_storage.cpp main/config_blob.cpp
    "$BUILD_DIR/nvs_storage_tests"
fi

# Display sim ↔ C++ presenter parity: confirm tools/display_sim.py's decide() still matches
# the firmware's tk::display::compose() (so the pixel sim can't silently drift from display.cpp).
# Skipped only where python3 is unavailable — the C++ logic tests above are the hard gate.
if command -v python3 >/dev/null 2>&1; then
    scripts/check-display-sim-parity.sh
    python3 test/test_provision.py
    python3 test/test_capture_wake.py
    python3 test/test_log_origin_contract.py
    scripts/test-build-contracts.sh
    scripts/apply-tesla-ble-patches.sh --self-test
    scripts/reconcile-pr-previews.sh --self-test
    scripts/verify-vendored-esptool-js.sh
else
    echo "run-mock-tests: python3 not found — skipping display-sim parity check" >&2
fi

# Browser checks: exercise the inline Web Serial installer and port-release flow, then confirm the
# BLE_ROW region of main/www/app.js still decides the Bluetooth row exactly as tk::ble::decide()
# does (so the browser can't silently drift from the host-tested rules). Skipped only where node is
# unavailable — the C++ logic tests are the hard gate; CI's ubuntu-latest runner ships node.
if command -v node >/dev/null 2>&1; then
    node --test test/serial_port_release.test.mjs test/web_installer.test.mjs test/web_ui_http.test.mjs
    scripts/check-ble-row-parity.sh
else
    echo "run-mock-tests: node not found — skipping web-installer and BLE-row checks" >&2
fi
