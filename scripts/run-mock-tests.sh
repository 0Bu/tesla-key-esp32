#!/usr/bin/env bash
# Host-side mock build: compile and run the IDF-free pure-logic tests (test/) with the
# system toolchain — no ESP-IDF, no Docker, no board. Catches logic regressions in
# seconds, in any environment (local terminal, CI, Claude Code web session). This is the
# real "run it and see" loop a cloud session has (it cannot build firmware or USB-flash).
#
# Usage: scripts/run-mock-tests.sh
# Requires: cmake + a C++17 host compiler (g++/clang++). See test/README.md.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR=build_mock   # matches .gitignore (/build_mock/)

cmake -S test -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure
