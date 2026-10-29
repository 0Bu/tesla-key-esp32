#!/usr/bin/env bash
# Host-side mock build: compile and run the IDF-free pure-logic tests (test/) with the
# system toolchain — no ESP-IDF or board. Strict macOS browser checks use Linux Docker. Catches logic regressions in
# seconds, in any environment (local terminal, CI, remote coding session). This is the
# real "run it and see" loop a cloud session has (it cannot build firmware or USB-flash).
#
# Usage: scripts/run-mock-tests.sh [--require-all]
# Requires: a C++17 host compiler (g++/clang++); cmake is used when present, with a
# direct-compiler fallback otherwise in normal mode (see test/CMakeLists.txt, whose targets
# and flags the fallback mirrors; --require-all strictly requires cmake). See test/README.md.
set -euo pipefail
if ! python3 -c 'import sys; raise SystemExit(sys.version_info < (3, 11))' >/dev/null 2>&1; then
    exec bash "$(cd "$(dirname "$0")" && pwd)/with-host-python.sh" bash "$0" "$@"
fi

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
    need_tool ruby
    need_tool jq
    gate_file "$TIMEOUT_RUNNER"
    for required in \
        scripts/check-display-sim-parity.sh \
        test/test_provision.py \
        test/test_capture_wake.py \
        test/test_log_origin_contract.py \
        test/test_diagnostic_export_guidance.py \
        test/test_runtime_boundary_contract.py \
        test/test_hook_read_contract.py \
        test/test_evcc_vehicle.rb \
        tools/agent-hooks/shell_read.py \
        test/generate_eth_startup_adapter.py \
        test/test_eth_startup_cleanup.cpp \
        test/test_eth_startup_cleanup.py \
        test/test_stack_context_hooks.py \
        test/stack_context_hook_owners.json \
        test/test_f137_host.py \
        test/f137_process.py \
        test/f137_process_controls.py \
        test/f137_driver_controls.py \
        test/f137_task_lsan.hpp \
        test/generate_f137_host_adapters.py \
        test/f137_mqtt_host_template.cpp \
        test/f137_net_host_template.cpp \
        test/f137_stubs/esp_task_wdt.h \
        test/f137_stubs/ping/ping_sock.h \
        test/f137-callback-probe-v1.py \
        test/f137-callback-probe-template-v1.cpp \
        test/test_f137_host_wiring.py \
        test/generate_heap_supervisor_adapter.py \
        test/test_heap_supervisor.cpp \
        test/test_stack_sample.cpp \
        main/stack_watch.hpp \
        main/stack_watch.cpp \
        main/logic/stack_context.hpp \
        main/logic/stack_sample_codec.hpp \
        test/stubs/esp_attr.h \
        test/test_charge_presentation.py \
        test/test_command_outcomes.py \
        test/test_inline_assets.py \
        main/www/prune_css_comments.py \
        test/generate_ota_admission_adapter.py \
        test/generate_mqtt_discovery_adapter.py \
        test/test_ota_admission.cpp \
        test/run-cjson-oom-tests.sh \
        test/ota_operation_contract.py \
        test/generate_ota_tls_adapter.py \
        test/test_ota_tls_admission.cpp \
        test/ota_http_parser/provenance.json \
        test/ota_http_parser/http_parser.c \
        test/ota_http_parser/http_parser.h \
        test/generate_ota_tls_adapter.py \
        test/test_ota_tls_admission.cpp \
        test/test_codex_setup.py \
        scripts/release-relevance.sh \
        scripts/test-build-contracts.sh \
        test/test_deploy_candidate.py \
        scripts/check-dependency-contract.py \
        scripts/check-bootloader-compat.py \
        scripts/bootloader-references.json \
        test/test_bootloader_preflight.py \
        scripts/check-otadata-contract.py \
        scripts/apply-tesla-ble-patches.sh \
        scripts/apply-tesla-ble-patches.py \
        scripts/tesla-ble-patch-state.json \
        test/test_patch_incremental.py \
        test/test_dependency_sources.py \
        test/test_dependency_cmake.py \
        test/test_dependency_harness.py \
        test/test_dependency_process.py \
        scripts/dependency_source.py \
        scripts/nanopb-source.json \
        cmake/admit-dependency-sources.cmake \
        test/test_dependency_guards.py \
        scripts/reconcile-pr-previews.sh \
        scripts/check-pr-preview-served.py \
        test/test_pr_preview_served.py \
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
        test/ble_row_golden_dump.cpp \
        test/display_golden_dump.cpp \
        test/test_display_parity_contract.py \
        test/test_led_driver_contract.py \
        test/config_repair_cases.hpp \
        test/generate_config_repair_backend.py \
        test/config_repair_backend_cases.json \
        test/config_repair_backend_omissions.json \
        test/config_repair_sdk/provenance.json \
        test/test_config_repair_backend.cpp \
        test/config_repair_retention_golden.hpp \
        test/config_repair_backend.cmake \
        test/run-config-repair-backend.sh \
        scripts/check-config-repair-backend-contract.py \
        test/config_repair_sdk/driver/uart.h \
        test/config_repair_sdk/driver/usb_serial_jtag.h \
        test/config_repair_sdk/esp_attr.h \
        test/config_repair_sdk/esp_err.h \
        test/config_repair_sdk/esp_mac.h \
        test/config_repair_sdk/esp_random.h \
        test/config_repair_sdk/esp_timer.h \
        test/config_repair_sdk/freertos/FreeRTOS.h \
        test/config_repair_sdk/freertos/task.h \
        test/config_repair_sdk/hal/uart_ll.h \
        test/config_repair_sdk/mbedtls/pk.h \
        test/config_repair_sdk/nvs.h \
        test/config_repair_sdk/nvs_flash.h \
        test/config_repair_sdk/psa/crypto.h \
        test/config_repair_sdk/sdkconfig.h \
        scripts/check-config-repair-stack.py \
        maintenance/config-repair/firmware-stack-baseline.json \
        test/generate_identity_recovery_adapter.py \
        test/test_identity_recovery.cpp \
        test/test_identity_recovery_contract.py \
        test/identity_recovery_process.py \
        test/identity_recovery_omission_cases.json \
        main/display_font.h \
        tools/ble_row_parity.js \
        scripts/run-browser-tests-linux.sh \
        tools/browser/Dockerfile \
        test/test_web_ui_browser_gate.py \
        test/test_browser_cdp.py \
        test/browser_cdp.py \
        test/web_ui_browser_gate.py; do
        gate_file "$required"
    done
fi

if command -v cmake >/dev/null 2>&1; then
    run_gate "configure host C++ tests" 120 cmake -S test -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
    run_gate "build host C++ tests" 120 cmake --build "$BUILD_DIR" --parallel
    run_gate "execute host C++ tests" 120 ctest --test-dir "$BUILD_DIR" --output-on-failure
else
    # No cmake on this host (e.g. a bare Raspberry Pi) — compile every host boundary directly.
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
        -pthread -o "$BUILD_DIR/nvs_storage_tests" \
        test/test_nvs_storage.cpp main/nvs_storage.cpp main/config_blob.cpp
    run_gate "execute NVS adapter tests" 120 "$BUILD_DIR/nvs_storage_tests"
    run_gate "generate background enqueue adapter" 120 python3 test/generate_background_poll_adapter.py \
        "$BUILD_DIR/background_poll_adapter.generated.hpp"
    run_gate "generate OTA admission adapter" 120 python3 test/generate_ota_admission_adapter.py \
        "$BUILD_DIR/ota_admission_adapter.generated.hpp"
    run_gate "compile runtime boundary tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Itest/stubs -Itest/coredump_stubs -Imain -I"$BUILD_DIR" -DTEST_BACKGROUND_POLL_ADAPTER=1 -DTEST_OTA_ADMISSION_ADAPTER=1 \
        -o "$BUILD_DIR/runtime_boundary_tests" \
        test/test_runtime_boundaries.cpp test/test_background_poll.cpp test/test_ota_admission.cpp test/test_coredump_hash.cpp main/diag_log.cpp main/stack_watch.cpp main/safe_mode.cpp
    run_gate "execute runtime boundary tests" 120 "$BUILD_DIR/runtime_boundary_tests"
    run_gate "generate heap supervisor adapters" 120 python3 test/generate_heap_supervisor_adapter.py \
        "$BUILD_DIR/heap_supervisor_adapter.generated.hpp"
    run_gate "compile heap supervisor tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Itest/stubs -Imain -I"$BUILD_DIR" -o "$BUILD_DIR/heap_supervisor_tests" \
        test/test_heap_supervisor.cpp main/safe_mode.cpp main/heap_trend.cpp main/stack_watch.cpp
    run_gate "execute heap supervisor tests" 120 "$BUILD_DIR/heap_supervisor_tests"
    run_gate "compile stack sample tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror -pthread \
        -Itest/stubs -Imain -o "$BUILD_DIR/stack_sample_tests" test/test_stack_sample.cpp
    run_gate "execute stack sample tests" 120 "$BUILD_DIR/stack_sample_tests"
    run_gate "generate clock shell adapters" 120 python3 test/generate_clock_adapters.py \
        "$BUILD_DIR/clock_adapters.generated.hpp"
    run_gate "compile clock shell tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Itest/stubs -Imain -I"$BUILD_DIR" -o "$BUILD_DIR/clock_runtime_tests" test/test_clock_runtime.cpp main/stack_watch.cpp
    run_gate "execute clock shell tests" 120 "$BUILD_DIR/clock_runtime_tests"
    run_gate "generate config shell adapters" 120 python3 test/generate_effect_reply_adapters.py \
        "$BUILD_DIR/effect_reply_adapters.generated.hpp"
    run_gate "compile pinned SDK query fixture" 120 "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itest -c test/fixtures/idf_ota_state_query.c -o "$BUILD_DIR/idf_ota_state_query.o"
    run_gate "compile config shell tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Itest -Itest/stubs -Imain -I"$BUILD_DIR" -o "$BUILD_DIR/config_commit_tests" \
        test/test_config_commit.cpp "$BUILD_DIR/idf_ota_state_query.o" main/stack_watch.cpp
    run_gate "execute config shell tests" 120 "$BUILD_DIR/config_commit_tests"
    run_gate "generate syslog shell adapters" 120 python3 test/generate_syslog_adapters.py \
        "$BUILD_DIR/syslog_adapters.generated.hpp"
    run_gate "compile syslog shell tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -Imain -I"$BUILD_DIR" -o "$BUILD_DIR/syslog_runtime_tests" test/test_syslog_runtime.cpp
    run_gate "execute syslog shell tests" 120 "$BUILD_DIR/syslog_runtime_tests"
    run_gate "generate Ethernet cleanup adapter" 120 python3 test/generate_eth_startup_adapter.py \
        "$BUILD_DIR/eth_startup_adapter.generated.hpp"
    run_gate "compile Ethernet cleanup tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
        -I"$BUILD_DIR" -o "$BUILD_DIR/eth_startup_cleanup_tests" test/test_eth_startup_cleanup.cpp
    run_gate "execute Ethernet cleanup tests" 120 "$BUILD_DIR/eth_startup_cleanup_tests"
    for crash_arch in xtensa riscv; do
        crash_flags=()
        if [ "$crash_arch" = riscv ]; then crash_flags=(-DHOST_CRASH_RISCV=1); fi
        run_gate "compile crash $crash_arch shell tests" 120 "$CXX" -std=c++17 -Wall -Wextra -Werror \
            ${crash_flags[@]+"${crash_flags[@]}"} -Itest/crash_stubs -Itest/coredump_stubs -Imain \
            -o "$BUILD_DIR/crash_runtime_${crash_arch}_tests" test/test_crash_runtime.cpp
        run_gate "execute crash $crash_arch shell tests" 120 "$BUILD_DIR/crash_runtime_${crash_arch}_tests"
    done
fi

# Display sim ↔ C++ presenter parity: confirm tools/display_sim.py's decide() still matches
# the firmware's tk::display::compose() (so the pixel sim can't silently drift from display.cpp).
# Skipped only where python3 is unavailable — the C++ logic tests above are the hard gate.
if command -v python3 >/dev/null 2>&1; then
    run_gate "display simulator parity" 120 scripts/check-display-sim-parity.sh
    run_gate "provisioning tests" 120 python3 test/test_provision.py
    run_gate "capture wake tests" 120 python3 test/test_capture_wake.py
    run_gate "evcc vehicle YAML and actual jq state mapping" 120 ruby test/test_evcc_vehicle.rb
    run_gate "log origin contract" 120 python3 test/test_log_origin_contract.py
    run_gate "diagnostic export guidance" 120 python3 test/test_diagnostic_export_guidance.py
    run_gate "stack context owner hooks" 120 python3 test/test_stack_context_hooks.py . --self-test
    run_gate "runtime C-boundary inventory" 120 python3 test/test_runtime_boundary_contract.py
    run_gate "Ethernet actual cleanup safety controls" 120 python3 test/test_eth_startup_cleanup.py
    run_gate "MQTT/WDT and network iteration bodies" 120 python3 test/test_f137_host.py --source . --controls --execute-controlled-host-only
    run_gate "coherent charge presentation snapshot" 120 python3 test/test_charge_presentation.py
    run_gate "contextual charging command outcomes" 120 python3 test/test_command_outcomes.py
    run_gate "CSS bytes and actual inline generation" 120 python3 test/test_inline_assets.py
    run_gate "optional LED driver decisions and GPIO" 120 python3 test/test_led_driver_contract.py
    run_gate "cached identity recovery and actual restart owner" 120 python3 test/test_identity_recovery_contract.py
    run_gate "locked cJSON intake and OOM" 120 bash test/run-cjson-oom-tests.sh
    run_gate "maintenance actual backend source contract" 120 python3 scripts/check-config-repair-backend-contract.py
    run_gate "maintenance actual backend and console" 120 bash test/run-config-repair-backend.sh
    run_gate "native Codex hook payload contract" 120 python3 test/test_codex_setup.py
    run_gate "bounded hook read exceptions" 120 python3 test/test_hook_read_contract.py
    run_gate "release relevance and publication selectors" 120 scripts/release-relevance.sh --self-test
    run_gate "build and manifest contract tests" 120 scripts/test-build-contracts.sh --build-and-manifest
    run_gate "retained bootloader compatibility and USB consumers" 120 python3 test/test_bootloader_preflight.py
    run_gate "exact pinned dependency contract" 120 \
        python3 scripts/check-dependency-contract.py --self-test
    run_gate "erased initial otadata contract" 120 \
        python3 scripts/check-otadata-contract.py --self-test
    run_gate "tesla-ble patch applicability" 120 scripts/apply-tesla-ble-patches.sh --self-test
    run_gate "tesla-ble incremental source admission" 120 python3 test/test_patch_incremental.py
run_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py
run_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py
run_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py
run_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py
    run_gate "preview reconciliation contract" 120 scripts/reconcile-pr-previews.sh --self-test
    run_gate "served PR preview and removal acceptance" 120 python3 test/test_pr_preview_served.py
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
    run_gate "browser launch policy" 120 python3 test/test_web_ui_browser_gate.py
    run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py
    run_gate "deterministic parser fuzz smoke" 120 scripts/run-fuzz-smoke.sh
else
    echo "run-mock-tests: python3 not found — skipping display-sim parity check" >&2
fi

# Browser checks: exercise the inline Web Serial installer and port-release flow, then confirm the
# BLE_ROW region of main/www/app.js still decides the Bluetooth row exactly as tk::ble::decide()
# does (so the browser can't silently drift from the host-tested rules). Skipped only where node is
# unavailable — the C++ logic tests are the hard gate; CI's ubuntu-26.04 runner ships node.
if command -v node >/dev/null 2>&1; then
    run_gate "Node web UI contracts" 120 node --test \
        test/serial_port_release.test.mjs test/web_installer.test.mjs test/web_ui_http.test.mjs \
        test/tesla_protocol_vectors.test.mjs
    run_gate "BLE browser-row parity" 120 scripts/check-ble-row-parity.sh
    if [ "$REQUIRE_ALL" = 1 ]; then
        if [ "$(uname -s)" = Darwin ]; then
            run_gate "Linux-container DOM and accessibility contract" 120 scripts/run-browser-tests-linux.sh
        else
            run_gate "real-browser DOM and accessibility contract" 120 \
                python3 test/web_ui_browser_gate.py --require-browser
        fi
    else
        python3 test/web_ui_browser_gate.py
    fi
else
    echo "run-mock-tests: node not found — skipping web-installer and BLE-row checks" >&2
fi
