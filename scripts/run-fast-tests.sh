#!/usr/bin/env bash
# Fast host-only test runner for tesla-key-esp32.
# Runs IDF-free pure-logic, NVS storage, runtime boundary, or sanitizer suites in seconds.
#
# Usage:
#   scripts/run-fast-tests.sh [--logic|--nvs|--boundary|--sanitizers|--all]
#   scripts/run-fast-tests.sh --self-test
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

BUILD_DIR=build_mock
mkdir -p "$BUILD_DIR"

self_test() {
  command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 || {
    echo "run-fast-tests: C++17 compiler required for self-test" >&2
    exit 2
  }
  echo "run-fast-tests: self-test PASS"
}

target="logic"
while [ "$#" -gt 0 ]; do
  case "$1" in
    --logic) target="logic"; shift ;;
    --nvs) target="nvs"; shift ;;
    --boundary) target="boundary"; shift ;;
    --sanitizers) target="sanitizers"; shift ;;
    --all) target="all"; shift ;;
    --self-test) self_test; exit 0 ;;
    *) echo "run-fast-tests: unknown argument: $1" >&2; exit 2 ;;
  esac
done

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
  if command -v g++ >/dev/null 2>&1; then CXX=g++
  elif command -v clang++ >/dev/null 2>&1; then CXX=clang++
  else
    echo "run-fast-tests: g++ or clang++ is required" >&2
    exit 1
  fi
fi

run_logic() {
  echo "== compiling and running pure-logic tests =="
  "$CXX" -std=c++17 -Wall -Wextra -Werror -Imain \
    -o "$BUILD_DIR/logic_tests" test/test_logic.cpp
  "$BUILD_DIR/logic_tests"
}

run_nvs() {
  echo "== compiling and running NVS storage adapter tests =="
  "$CXX" -std=c++17 -Wall -Wextra -Werror -Itest/stubs -Imain \
    -o "$BUILD_DIR/nvs_storage_tests" \
    test/test_nvs_storage.cpp main/nvs_storage.cpp main/config_blob.cpp
  "$BUILD_DIR/nvs_storage_tests"
}

run_boundary() {
  echo "== compiling and running runtime boundary tests =="
  "$CXX" -std=c++17 -Wall -Wextra -Werror -Itest/stubs -Imain \
    -o "$BUILD_DIR/runtime_boundary_tests" \
    test/test_runtime_boundaries.cpp main/diag_log.cpp main/safe_mode.cpp
  "$BUILD_DIR/runtime_boundary_tests"
}

run_sanitizers() {
  echo "== running sanitizer suite =="
  ./scripts/run-sanitizer-tests.sh
}

case "$target" in
  logic) run_logic ;;
  nvs) run_nvs ;;
  boundary) run_boundary ;;
  sanitizers) run_sanitizers ;;
  all)
    run_logic
    run_nvs
    run_boundary
    run_sanitizers
    ;;
esac
