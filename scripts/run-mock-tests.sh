#!/usr/bin/env bash
# Host-side mock build: compile and run the IDF-free pure-logic tests (test/) with the
# system toolchain — no ESP-IDF, no Docker, no board. Catches logic regressions in
# seconds, in any environment (local terminal, CI, Claude Code web session). This is the
# real "run it and see" loop a cloud session has (it cannot build firmware or USB-flash).
#
# Usage: scripts/run-mock-tests.sh
# Requires: a C++17 host compiler (g++/clang++); cmake is used when present, with a
# direct-compiler fallback otherwise (the suite is one translation unit — see
# test/CMakeLists.txt, whose flags the fallback mirrors). See test/README.md.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR=build_mock   # matches .gitignore (/build_mock/)

if command -v cmake >/dev/null 2>&1; then
    cmake -S test -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug >/dev/null
    cmake --build "$BUILD_DIR" --parallel
    ctest --test-dir "$BUILD_DIR" --output-on-failure
else
    # No cmake on this host (e.g. a bare Raspberry Pi) — the suite is a single
    # translation unit, so compile it directly. Keep flags in sync with test/CMakeLists.txt.
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
fi

# Display sim ↔ C++ presenter parity: confirm tools/display_sim.py's decide() still matches
# the firmware's tk::display::compose() (so the pixel sim can't silently drift from display.cpp).
# Skipped only where python3 is unavailable — the C++ logic tests above are the hard gate.
if command -v python3 >/dev/null 2>&1; then
    scripts/check-display-sim-parity.sh
else
    echo "run-mock-tests: python3 not found — skipping display-sim parity check" >&2
fi

# Web UI ↔ C++ presenter parity: confirm the BLE_ROW region of main/www/app.js still decides the
# Bluetooth row exactly as tk::ble::decide() does (so the browser can't silently drift from the
# host-tested rules). Skipped only where node is unavailable — the C++ logic tests are the hard
# gate; CI's ubuntu-latest runner ships node, so the check does run there.
if command -v node >/dev/null 2>&1; then
    scripts/check-ble-row-parity.sh
else
    echo "run-mock-tests: node not found — skipping BLE-row parity check" >&2
fi
