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
  "$CXX" -std=c++17 -Wall -Wextra -Werror -Itest/stubs -Imain -pthread \
    -o "$BUILD_DIR/nvs_storage_tests" \
    test/test_nvs_storage.cpp main/nvs_storage.cpp main/config_blob.cpp
  "$BUILD_DIR/nvs_storage_tests"
}

run_boundary() {
  echo "== compiling and running runtime boundary tests =="
  python3 test/generate_background_poll_adapter.py "$BUILD_DIR/background_poll_adapter.generated.hpp"
  python3 test/generate_ota_admission_adapter.py "$BUILD_DIR/ota_admission_adapter.generated.hpp"
  "$CXX" -std=c++17 -Wall -Wextra -Werror -Itest/stubs -Itest/coredump_stubs -Imain -I"$BUILD_DIR" -DTEST_BACKGROUND_POLL_ADAPTER=1 -DTEST_OTA_ADMISSION_ADAPTER=1 \
    -o "$BUILD_DIR/runtime_boundary_tests" \
    test/test_runtime_boundaries.cpp test/test_background_poll.cpp test/test_ota_admission.cpp test/test_coredump_hash.cpp main/diag_log.cpp main/stack_watch.cpp main/safe_mode.cpp
  "$BUILD_DIR/runtime_boundary_tests"
  python3 test/generate_syslog_adapters.py "$BUILD_DIR/syslog_adapters.generated.hpp"
  "$CXX" -std=c++17 -Wall -Wextra -Werror -Imain -I"$BUILD_DIR" \
    -o "$BUILD_DIR/syslog_runtime_tests" test/test_syslog_runtime.cpp
  "$BUILD_DIR/syslog_runtime_tests"
  for crash_arch in xtensa riscv; do
    crash_flags=()
    if [ "$crash_arch" = riscv ]; then crash_flags=(-DHOST_CRASH_RISCV=1); fi
    "$CXX" -std=c++17 -Wall -Wextra -Werror ${crash_flags[@]+"${crash_flags[@]}"} \
      -Itest/crash_stubs -Itest/coredump_stubs -Imain \
      -o "$BUILD_DIR/crash_runtime_${crash_arch}_tests" test/test_crash_runtime.cpp
    "$BUILD_DIR/crash_runtime_${crash_arch}_tests"
  done
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
