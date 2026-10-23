#!/usr/bin/env python3
"""Static and mutation-tested fail-closed contract for CI/Stop host gates."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import sys
import tempfile
import json
import os
import subprocess


class ContractError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def probe_sanitizer_invocations(source: str) -> None:
    """Execute shell control flow with passive tools, not a second sanitizer run."""
    with tempfile.TemporaryDirectory(prefix="sanitizer-flow-") as directory:
        work = Path(directory)
        scripts, binaries, scratch = work / "scripts", work / "bin", work / "tmp"
        for path in (scripts, binaries, scratch):
            path.mkdir()
        script = scripts / "run-sanitizer-tests.sh"
        script.write_text(source)
        trace = work / "calls.jsonl"
        # The fake compiler/CMake only produce successful disposable executables. The
        # actual shell must still traverse its prerequisites, loops and two Python calls.
        stub = '''#!PYTHON
import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
if name == "python3":
    with open(os.environ["TESLA_FLOW_TRACE"], "a") as out:
        out.write(json.dumps(args) + "\\n")
elif name == "cmake" and "-B" in args:
    build = Path(args[args.index("-B") + 1]); build.mkdir(parents=True, exist_ok=True)
    for target in ("logic_tests", "nvs_storage_tests", "runtime_boundary_tests", "clock_runtime_tests",
                   "config_commit_tests", "syslog_runtime_tests", "crash_runtime_xtensa_tests", "crash_runtime_riscv_tests"):
        output = build / target; output.write_text("#!/bin/sh\\nexit 0\\n"); output.chmod(0o700)
elif name == "compiler" and "-o" in args:
    output = Path(args[args.index("-o") + 1])
    if str(output) != "/dev/null":
        output.write_text("#!/bin/sh\\nexit 0\\n"); output.chmod(0o700)
'''.replace("#!PYTHON", "#!" + sys.executable)
        for name in ("cmake", "compiler", "python3"):
            path = binaries / name
            path.write_text(stub)
            path.chmod(0o700)
        uname = binaries / "uname"
        uname.write_text("#!/bin/sh\nprintf 'Linux\\n'\n")
        uname.chmod(0o700)
        env = os.environ.copy()
        for name in list(env):
            if name in ("OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "ENV", "BASH_ENV") or name.startswith("BASH_FUNC_"):
                env.pop(name, None)
        env.update(PATH=str(binaries) + os.pathsep + env.get("PATH", os.defpath),
                   CC=str(binaries / "compiler"), CXX=str(binaries / "compiler"),
                   TMPDIR=str(scratch), TESLA_FLOW_TRACE=str(trace))
        try:
            result = subprocess.run(["/bin/bash", str(script)], env=env, cwd=work,
                                    capture_output=True, text=True, timeout=10)
        except subprocess.TimeoutExpired as exc:
            raise ContractError("sanitizer invocation flow timed out") from exc
        calls = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
        require(result.returncode == 0 and calls == [
            ["test/test_led_driver_contract.py", "--sanitize"],
            ["test/test_display_parity_contract.py", "--render-only", "--sanitize"],
        ], "sanitizer invocation flow did not execute both actual native-probe commands")


def validate(root: Path) -> None:
    run_mock = (root / "scripts/run-mock-tests.sh").read_text(encoding="utf-8")
    idf_verify = (root / "scripts/ci-build-verify.sh").read_text(encoding="utf-8")
    hook = (root / "tools/agent-hooks/agent_hook.py").read_text(encoding="utf-8")
    cmake = (root / "test/CMakeLists.txt").read_text(encoding="utf-8")
    sanitizers = (root / "scripts/run-sanitizer-tests.sh").read_text(encoding="utf-8")
    browser_gate = (root / "test/web_ui_browser_gate.py").read_text(encoding="utf-8")
    fast = (root / "scripts/run-fast-tests.sh").read_text(encoding="utf-8")
    mqtt_gate_source = (root / "test/run-mqtt-json-publish-tests.sh").read_text(encoding="utf-8")
    mqtt_test = (root / "test/test_mqtt_json_publish.cpp").read_text(encoding="utf-8")
    require('python3 "$repo_root/test/generate_mqtt_discovery_adapter.py" "$work/mqtt_discovery_adapter.generated.hpp"' in mqtt_gate_source and
            '-I"$work"' in mqtt_gate_source and
            '#include "mqtt_discovery_adapter.generated.hpp"' in mqtt_test and
            '    test_current_discovery_address();' in mqtt_test,
            "actual MQTT discovery address fixture is missing from the retained JSON gate")
    publication_gate = (root / "scripts/test-build-contracts.sh").read_text()
    for validator in ("check-pages-manifest.py", "check-release-pages-bytes.py"):
        require(f'python3 "$repo_root/scripts/{validator}" --self-test' in publication_gate,
                f"publication validator self-test is not executed: {validator}")
    display_gate = (root / "scripts/check-display-sim-parity.sh").read_text()
    require('"$OUT/display_golden_dump" --font' in display_gate and
            'python3 tools/display_sim.py font-parity "$OUT/display_font_golden.tsv"' in display_gate and
            'cmp main/display_font.h "$OUT/display_font.generated.h"' in display_gate and
            'python3 test/test_display_parity_contract.py' in display_gate and
            'test/test_display_parity_contract.py' in run_mock,
            "display native font/provenance/golden controls are missing from the strict host gate")
    require('run_gate "optional LED driver decisions and GPIO" 120 python3 test/test_led_driver_contract.py' in run_mock and
            'test/test_led_driver_contract.py' in run_mock and
            'python3 test/test_led_driver_contract.py --sanitize' in sanitizers,
            "actual optional LED driver gate is missing")
    require('python3 test/test_display_parity_contract.py --render-only --sanitize' in sanitizers,
            "actual display renderer sanitizer gate is missing")
    probe_sanitizer_invocations(sanitizers)
    presenter_gate = (root / "scripts/check-ble-row-parity.sh").read_text()
    require('"$OUT/ble_row_golden_dump" --web-presenters > "$OUT/web_presenter_golden.tsv"' in presenter_gate and
            'node tools/ble_row_parity.js "$OUT/ble_row_golden.tsv" "$OUT/web_presenter_golden.tsv"' in presenter_gate and
            'run_gate "BLE browser-row parity" 120 scripts/check-ble-row-parity.sh' in run_mock and
            "test/ble_row_golden_dump.cpp" in run_mock and "tools/ble_row_parity.js" in run_mock,
            "actual web presenter native/glyph parity is missing from the strict host gate")
    require("--require-all" in run_mock and 'REQUIRE_ALL=1' in run_mock,
            "run-mock-tests.sh has no strict mode")
    for tool in ("python3", "node", "cmake"):
        require(f"need_tool {tool}" in run_mock,
                f"run-mock-tests.sh strict mode does not require {tool}")
    require("tools/agent-hooks/run_with_timeout.py" in run_mock and "run_gate" in run_mock,
            "run-mock-tests.sh strict sub-gates are not bounded")
    for path in (
        "test/tesla_protocol_vectors.test.mjs",
        "test/test_runtime_boundary_contract.py",
        "test/test_charge_presentation.py",
        "test/run-cjson-oom-tests.sh",
        "test/test_codex_setup.py",
        "scripts/check-bench-acceptance.py",
        "scripts/check-dependency-contract.py",
        "scripts/check-bootloader-compat.py",
        "scripts/bootloader-references.json",
        "test/test_bootloader_preflight.py",
        "test/test_patch_incremental.py",
        "test/test_dependency_guards.py",
        "scripts/check-host-gate-contract.py",
        "scripts/check-logic-test-ownership.py",
        "scripts/check-nvs-contract.py",
        "scripts/check-otadata-contract.py",
        "scripts/prepare-reused-release.py",
        "scripts/run-fuzz-smoke.sh",
        "test/web_ui_browser_gate.py",
        "test/test_web_ui_browser_gate.py",
    ):
        require(path in run_mock, f"run-mock-tests.sh strict inventory omits {path}")
    require("python3 scripts/check-logic-test-ownership.py --compile --self-test" in run_mock,
            "run-mock-tests.sh does not execute the exact logic ownership/compile gate")
    require('run_gate "locked cJSON intake and OOM" 120 bash test/run-cjson-oom-tests.sh' in run_mock,
            "strict host gate must execute the locked cJSON intake/OOM tests")
    require("python3 scripts/check-host-gate-contract.py" in run_mock,
            "run-mock-tests.sh does not execute the host wiring contract")
    require('run_gate "coherent charge presentation snapshot" 120 python3 test/test_charge_presentation.py' in run_mock,
            "strict host gate must execute the coherent charge presentation snapshot")
    require('run_gate "retained bootloader compatibility and USB consumers" 120 python3 test/test_bootloader_preflight.py' in run_mock,
            "strict host gate must execute retained-bootloader and USB-consumer admission controls")
    require("python3 scripts/check-nvs-contract.py --self-test" in run_mock,
            "run-mock-tests.sh does not execute the NVS contract gate")
    require("python3 scripts/check-dependency-contract.py --self-test" in run_mock,
            "run-mock-tests.sh does not execute the dependency contract gate")
    require('run_gate "tesla-ble incremental source admission" 120 python3 test/test_patch_incremental.py' in run_mock,
            "run-mock-tests.sh does not execute incremental patch admission")
    require("python3 scripts/check-otadata-contract.py --self-test" in run_mock,
            "run-mock-tests.sh does not execute the otadata contract gate")
    require("python3 scripts/prepare-reused-release.py --self-test" in run_mock,
            "run-mock-tests.sh does not execute the immutable Release reuse gate")
    require("        scripts/check-dependency-contract.py \\\n" in run_mock and
            "        scripts/check-otadata-contract.py \\\n" in run_mock and
            "        scripts/prepare-reused-release.py \\\n" in run_mock,
            "run-mock-tests.sh strict gate_file inventory omits dependency/otadata/reuse validators")
    require('[str(script), "--require-all"]' in hook,
            "Stop hook does not invoke the strict host gate")
    require('("git", "python3", "cmake", "node")' in hook,
            "Stop hook tool inventory is incomplete")
    require("script.is_symlink()" in hook and "os.access(script, os.X_OK)" in hook,
            "Stop hook does not fail closed for a missing/unsafe script")
    require("except subprocess.TimeoutExpired" in hook and "except OSError" in hook,
            "Stop hook does not block timeout/start failures")
    require("COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_background_poll_adapter.py" in cmake and
            "target_sources(runtime_boundary_tests PRIVATE test_background_poll.cpp" in cmake and
            "TEST_BACKGROUND_POLL_ADAPTER=1" in cmake,
            "actual background enqueue seam is missing from native runtime tests")
    for entry, label in ((run_mock, 'cmake-less fallback'), (fast, 'fast boundary')):
        require('python3 test/generate_background_poll_adapter.py' in entry and
                'test/test_background_poll.cpp' in entry and 'TEST_BACKGROUND_POLL_ADAPTER=1' in entry,
                label + " omits the actual background enqueue seam")
    require("add_executable(runtime_boundary_tests" in cmake,
            "runtime_boundary_tests target is missing from host CMake")
    require("COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_ota_admission_adapter.py" in cmake and
            "target_sources(runtime_boundary_tests PRIVATE test_ota_admission.cpp" in cmake and
            "TEST_OTA_ADMISSION_ADAPTER=1" in cmake,
            "actual OTA admission/cache seam is missing from native runtime tests")
    for entry, label in ((run_mock, 'cmake-less fallback'), (fast, 'fast boundary')):
        require('python3 test/generate_ota_admission_adapter.py' in entry and
                'test/test_ota_admission.cpp' in entry and 'TEST_OTA_ADMISSION_ADAPTER=1' in entry,
                label + " omits the actual OTA admission/cache seam")
    require("add_test(NAME runtime_boundary_tests" in cmake,
            "runtime_boundary_tests is not part of ctest")
    require("add_executable(nvs_storage_tests" in cmake and
            "test_nvs_storage.cpp" in cmake and
            "../main/nvs_storage.cpp" in cmake and
            "../main/config_blob.cpp" in cmake,
            "nvs_storage_tests target/source wiring is incomplete")
    require("find_package(Threads REQUIRED)" in cmake and
            "target_link_libraries(nvs_storage_tests PRIVATE Threads::Threads)" in cmake and
            '-pthread -o "$BUILD_DIR/nvs_storage_tests"' in run_mock,
            "NVS concurrency tests require explicit thread linkage in CMake and fallback")
    require("add_test(NAME nvs_storage_tests COMMAND nvs_storage_tests)" in cmake,
            "nvs_storage_tests is not part of ctest")
    require("add_executable(clock_runtime_tests" in cmake and
            "test_clock_runtime.cpp" in cmake and
            "COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_clock_adapters.py" in cmake and
            "../main/main.cpp ../main/vehicle_pairing.cpp" in cmake,
            "clock_runtime_tests target/source generation is incomplete")
    require("add_test(NAME clock_runtime_tests COMMAND clock_runtime_tests)" in cmake,
            "clock_runtime_tests is not part of ctest")
    require('run_gate "execute clock shell tests" 120 "$BUILD_DIR/clock_runtime_tests"' in run_mock and
            'python3 test/generate_clock_adapters.py' in run_mock and
            'test/test_clock_runtime.cpp' in run_mock,
            "cmake-less fallback omits the actual clock shell")
    require("add_executable(config_commit_tests" in cmake and
            "fixtures/idf_ota_state_query.c" in cmake and "../main/provisioning.cpp" in cmake and
            "add_test(NAME config_commit_tests COMMAND config_commit_tests)" in cmake,
            "config_commit_tests SDK/portal generation or CTest is incomplete")
    config_generator = (root / "test/generate_effect_reply_adapters.py").read_text()
    config_test = (root / "test/test_config_commit.cpp").read_text()
    require('function("main.cpp", "ota_health_gate_task")' in config_generator and
            'runtime.index("OtaHealthCommitGuard::OtaHealthCommitGuard()")' in config_generator and
            'function("ota_update.cpp", "finish_operation")' in config_generator and
            '#include "ota_health_task.generated.hpp"' in config_test and
            '    timed_health_tests();' in config_test and
            'CHECK(config_fixture_runs==1 && timed_fixture_runs==1);' in config_test,
            "actual timed/config health consumer extraction or execution is missing")
    config_dependency = cmake[cmake.index('OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/config_commit_runtime.generated.hpp'):]
    config_dependency = config_dependency[:config_dependency.index('VERBATIM)')]
    require('../main/main.cpp' in config_dependency and '../main/logic/health_gate.hpp' in config_dependency and
            '../main/logic/config_commit.hpp' in config_dependency and
            '${CMAKE_CURRENT_BINARY_DIR}/ota_health_task.generated.hpp' in config_dependency,
            "actual health adapter regeneration dependencies are missing")
    require('run_gate "execute config shell tests" 120 "$BUILD_DIR/config_commit_tests"' in run_mock and
            'python3 test/generate_effect_reply_adapters.py' in run_mock and
            'test/test_config_commit.cpp "$BUILD_DIR/idf_ota_state_query.o"' in run_mock,
            "cmake-less fallback omits the config/SDK shell")
    configure_blocks = []
    for opener in ('cmake -S test -B "$work/build"', 'cmake -S "$work/mixed" -B "$work/mixed-build"'):
        start = sanitizers.find(opener)
        end = sanitizers.find('cmake --build', start)
        require(start >= 0 and end > start,
                "config/SDK shell C and C++ sanitizer configure block is missing")
        configure_blocks.append("\n".join(line for line in sanitizers[start:end].splitlines()
                                            if not line.lstrip().startswith("#")))
    require(all(all(flag in block for flag in ('-DCMAKE_C_FLAGS="${c_flags[*]}"',
                                               '-DCMAKE_C_COMPILER="$CC"',
                                               '-DCMAKE_CXX_COMPILER="$CXX"'))
                for block in configure_blocks) and
            '--target logic_tests nvs_storage_tests runtime_boundary_tests clock_runtime_tests config_commit_tests syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests' in sanitizers and
            'for target in logic_tests nvs_storage_tests runtime_boundary_tests clock_runtime_tests config_commit_tests syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests; do' in sanitizers,
            "config/SDK shell C and C++ sanitizer instrumentation/execution is incomplete")
    require("add_executable(syslog_runtime_tests test_syslog_runtime.cpp" in cmake and
            "COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_syslog_adapters.py" in cmake and
            "test_runtime_boundary_contract.py ../main/syslog.cpp" in cmake and
            "add_test(NAME syslog_runtime_tests COMMAND syslog_runtime_tests)" in cmake,
            "syslog runtime target/source generation or CTest is incomplete")
    require('run_gate "execute syslog shell tests" 120 "$BUILD_DIR/syslog_runtime_tests"' in run_mock and
            'python3 test/generate_syslog_adapters.py' in run_mock and
            'test/test_syslog_runtime.cpp' in run_mock,
            "cmake-less fallback omits the actual syslog shell")
    require('test/test_coredump_hash.cpp' in fast and '-Itest/stubs -Itest/coredump_stubs -Imain' in fast and
            'python3 test/generate_syslog_adapters.py' in fast and
            'test/test_syslog_runtime.cpp' in fast and '"$BUILD_DIR/syslog_runtime_tests"' in fast,
            "fast boundary checksum/syslog adapter wiring is incomplete")
    require('foreach(crash_arch IN ITEMS xtensa riscv)' in cmake and
            'add_executable(${crash_target} test_crash_runtime.cpp)' in cmake and
            'add_test(NAME ${crash_target} COMMAND ${crash_target})' in cmake and
            'HOST_CRASH_RISCV=1' in cmake,
            "actual crash shell architecture targets or CTest are incomplete")
    for entry, label in ((run_mock, 'cmake-less fallback'), (fast, 'fast boundary')):
        require('for crash_arch in xtensa riscv; do' in entry and
                'test/test_crash_runtime.cpp' in entry and '-Itest/crash_stubs' in entry and
                'HOST_CRASH_RISCV=1' in entry and
                '"$BUILD_DIR/crash_runtime_${crash_arch}_tests"' in entry,
                label + " omits the actual crash shell")
    require('-Itest/stubs -Imain -pthread' in fast,
            "fast NVS concurrency tests require explicit thread linkage")
    require("../main/stack_watch.cpp" in cmake and "main/stack_watch.cpp" in fast,
            "actual stack minimum shell is missing from CMake/fast runtime tests")
    for source in (
        "test/test_runtime_boundaries.cpp",
        "main/diag_log.cpp",
        "main/stack_watch.cpp",
        "main/safe_mode.cpp",
    ):
        require(source in run_mock,
                f"cmake-less runtime_boundary_tests fallback omits {source}")
    require('-Itest/stubs -Imain' in run_mock and
            '"$CXX" -std=c++17 -Wall -Wextra -Werror' in run_mock,
            "cmake-less fallback does not mirror host include/warning flags")
    require('-o "$BUILD_DIR/nvs_storage_tests"' in run_mock and
            "test/test_nvs_storage.cpp main/nvs_storage.cpp main/config_blob.cpp" in run_mock and
            'run_gate "execute NVS adapter tests" 120 "$BUILD_DIR/nvs_storage_tests"' in run_mock,
            "cmake-less fallback does not compile and execute nvs_storage_tests")
    require(
        'run_gate "real-browser DOM and accessibility contract" 120 \\\n'
        "                python3 test/web_ui_browser_gate.py --require-browser" in run_mock,
        "strict real-browser gate must retain its 120-second outer timeout",
    )
    require(
        "PROFILE_TIMEOUT_SECONDS = 45" in browser_gate
        and browser_gate.count("process.wait(timeout=PROFILE_TIMEOUT_SECONDS)") == 1
        and "TERMINATION_GRACE_SECONDS = 2" in browser_gate
        and browser_gate.count("process.wait(timeout=TERMINATION_GRACE_SECONDS)") == 2,
        "real-browser gate cold-start and termination budgets drifted",
    )
    require('run_gate "browser launch policy" 120 python3 test/test_web_ui_browser_gate.py' in run_mock,
            "host suite must execute browser launch-policy tests")
    require('if [ "$(uname -s)" = Darwin ]; then' in run_mock and
            'run_gate "Linux-container DOM and accessibility contract" 120 scripts/run-browser-tests-linux.sh' in run_mock,
            "strict macOS browser gate must execute its Linux container")
    require(browser_gate.count('if sys.platform != "linux":') == 3,
            "Linux-only browser launch guards drifted")
    cjson_gate = "CJSON_OOM_SANITIZE=1 bash ./test/run-cjson-oom-tests.sh"
    mqtt_gate = "MQTT_JSON_SANITIZE=1 bash ./test/run-mqtt-json-publish-tests.sh"
    build = './scripts/ci-build-all.sh "$version" "$source_sha"'
    require(cjson_gate in idf_verify and mqtt_gate in idf_verify,
            "pinned IDF orchestration omits sanitized real-cJSON host gates")
    require(idf_verify.index(cjson_gate) < idf_verify.index(build) and
            idf_verify.index(mqtt_gate) < idf_verify.index(build),
            "real-cJSON host gates must run before expensive firmware builds")


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    require(old in text, f"self-test fixture text is absent: {old}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def self_test(root: Path) -> None:
    validate(root)
    mutations = [
        ("scripts/run-mock-tests.sh", 'run_gate "coherent charge presentation snapshot" 120 python3 test/test_charge_presentation.py',
         'true # removed charge snapshot gate', "coherent charge presentation snapshot"),
        ("scripts/run-mock-tests.sh", 'run_gate "optional LED driver decisions and GPIO" 120 python3 test/test_led_driver_contract.py',
         '# removed LED gate', "actual optional LED driver gate is missing"),
        ("scripts/run-sanitizer-tests.sh", 'python3 test/test_led_driver_contract.py --sanitize',
         'true # removed LED sanitizers', "actual optional LED driver gate is missing"),
        ("scripts/check-display-sim-parity.sh", 'cmp main/display_font.h "$OUT/display_font.generated.h"',
         '# removed font provenance', "display native font/provenance/golden controls"),
        ("scripts/check-display-sim-parity.sh", 'python3 test/test_display_parity_contract.py',
         '# removed empty golden controls', "display native font/provenance/golden controls"),
        ("scripts/test-build-contracts.sh", 'python3 "$repo_root/scripts/check-pages-manifest.py" --self-test',
         '# removed Pages self-test', "publication validator self-test is not executed"),
        ("scripts/test-build-contracts.sh", 'python3 "$repo_root/scripts/check-release-pages-bytes.py" --self-test',
         '# removed byte-binding self-test', "publication validator self-test is not executed"),
        ("scripts/check-ble-row-parity.sh", '"$OUT/ble_row_golden_dump" --web-presenters',
         '"$OUT/ble_row_golden_dump" --missing-web-presenters', "actual web presenter native/glyph parity"),
        ("scripts/check-ble-row-parity.sh", '"$OUT/ble_row_golden.tsv" "$OUT/web_presenter_golden.tsv"',
         '"$OUT/ble_row_golden.tsv"', "actual web presenter native/glyph parity"),
        ("test/run-mqtt-json-publish-tests.sh", 'python3 "$repo_root/test/generate_mqtt_discovery_adapter.py"',
         'python3 missing_discovery.py', "actual MQTT discovery address fixture"),
        ("test/test_mqtt_json_publish.cpp", '    test_current_discovery_address();',
         '    // omitted address fixture', "actual MQTT discovery address fixture"),
        ("test/CMakeLists.txt", "target_sources(runtime_boundary_tests PRIVATE test_ota_admission.cpp",
         "target_sources(runtime_boundary_tests PRIVATE missing_ota.cpp", "actual OTA admission/cache seam"),
        ("scripts/run-mock-tests.sh", "python3 test/generate_ota_admission_adapter.py",
         "python3 missing_ota_generator.py", "cmake-less fallback omits the actual OTA admission/cache seam"),
        ("scripts/run-fast-tests.sh", "-DTEST_OTA_ADMISSION_ADAPTER=1",
         "-DTEST_OTA_ADMISSION_ADAPTER=0", "fast boundary omits the actual OTA admission/cache seam"),
        ("scripts/run-mock-tests.sh", 'run_gate "retained bootloader compatibility and USB consumers" 120 python3 test/test_bootloader_preflight.py',
         'true # omitted bootloader gate', "retained-bootloader and USB-consumer admission"),
        ("test/CMakeLists.txt", "add_test(NAME ${crash_target} COMMAND ${crash_target})",
         "# removed crash CTest", "actual crash shell architecture"),
        ("test/CMakeLists.txt", "foreach(crash_arch IN ITEMS xtensa riscv)",
         "foreach(crash_arch IN ITEMS xtensa)", "actual crash shell architecture"),
        ("scripts/run-mock-tests.sh", "-Itest/crash_stubs", "-Imissing_crash_stubs",
         "cmake-less fallback omits the actual crash"),
        ("scripts/run-fast-tests.sh", "test/test_crash_runtime.cpp", "missing_crash.cpp",
         "fast boundary omits the actual crash"),
        ("scripts/run-sanitizer-tests.sh", "syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests; do",
         "syslog_runtime_tests; do", "config/SDK shell C and C++ sanitizer"),
        ("test/CMakeLists.txt", "add_test(NAME syslog_runtime_tests COMMAND syslog_runtime_tests)",
         "# removed syslog CTest", "syslog runtime target/source"),
        ("test/CMakeLists.txt", "COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_syslog_adapters.py",
         "COMMAND missing_syslog_generator", "syslog runtime target/source"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute syslog shell tests" 120 "$BUILD_DIR/syslog_runtime_tests"',
         "# removed syslog fallback", "cmake-less fallback omits the actual syslog"),
        ("scripts/run-sanitizer-tests.sh", "config_commit_tests syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests; do",
         "config_commit_tests; do", "config/SDK shell C and C++ sanitizer"),
        ("scripts/run-fast-tests.sh", "test/test_coredump_hash.cpp", "missing_hash.cpp",
         "fast boundary checksum/syslog"),
        ("scripts/run-fast-tests.sh", "-Itest/coredump_stubs", "-Imissing_stubs",
         "fast boundary checksum/syslog"),
        ("scripts/run-fast-tests.sh", "-pthread", "", "fast NVS concurrency"),
        ("test/generate_effect_reply_adapters.py", 'function("main.cpp", "ota_health_gate_task")',
         'function("main.cpp", "missing_task")', "actual timed/config health consumer extraction"),
        ("test/test_config_commit.cpp", '    timed_health_tests();',
         '    // omitted actual timed health tests', "actual timed/config health consumer extraction"),
        ("test/test_config_commit.cpp", '#include "ota_health_task.generated.hpp"',
         '// omitted actual timed health header', "actual timed/config health consumer extraction"),
        ("test/CMakeLists.txt", '../main/ota_update.cpp ../main/main.cpp',
         '../main/ota_update.cpp', "actual health adapter regeneration dependencies"),
        ("test/CMakeLists.txt", "add_test(NAME config_commit_tests COMMAND config_commit_tests)",
         "# removed config CTest", "config_commit_tests SDK/portal generation"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute config shell tests" 120 "$BUILD_DIR/config_commit_tests"',
         "# removed config fallback", "cmake-less fallback omits the config/SDK shell"),
        ("scripts/run-sanitizer-tests.sh", '-DCMAKE_C_FLAGS="${c_flags[*]}"',
         "# removed C sanitizer instrumentation", "config/SDK shell C and C++ sanitizer"),
        ("scripts/run-sanitizer-tests.sh", 'cmake -S test -B "$work/build" -DCMAKE_BUILD_TYPE=Debug \\\n    -DCMAKE_C_COMPILER="$CC"',
         'cmake -S test -B "$work/build" -DCMAKE_BUILD_TYPE=Debug \\\n    -DCMAKE_C_COMPILER=gcc', "config/SDK shell C and C++ sanitizer"),
        ("scripts/run-sanitizer-tests.sh", 'cmake -S test -B "$work/build" -DCMAKE_BUILD_TYPE=Debug \\\n    -DCMAKE_C_COMPILER="$CC" \\\n    -DCMAKE_CXX_COMPILER="$CXX" \\\n    -DCMAKE_CXX_FLAGS="${flags[*]}" \\\n    -DCMAKE_C_FLAGS="${c_flags[*]}"',
         'cmake -S test -B "$work/build" -DCMAKE_BUILD_TYPE=Debug \\\n    -DCMAKE_C_COMPILER="$CC" \\\n    -DCMAKE_CXX_COMPILER="$CXX" \\\n    -DCMAKE_CXX_FLAGS="${flags[*]}"', "config/SDK shell C and C++ sanitizer"),
        ("test/CMakeLists.txt", "target_link_libraries(nvs_storage_tests PRIVATE Threads::Threads)",
         "# removed NVS threads", "NVS concurrency tests require"),
        ("scripts/run-mock-tests.sh", '-pthread -o "$BUILD_DIR/nvs_storage_tests"',
         '-o "$BUILD_DIR/nvs_storage_tests"', "NVS concurrency tests require"),
        ("test/CMakeLists.txt", "add_test(NAME clock_runtime_tests COMMAND clock_runtime_tests)",
         "# removed clock CTest", "clock_runtime_tests is not part"),
        ("test/CMakeLists.txt", "generate_clock_adapters.py", "missing_clock_adapters.py",
         "clock_runtime_tests target/source generation"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute clock shell tests" 120 "$BUILD_DIR/clock_runtime_tests"',
         "# removed clock fallback execution", "cmake-less fallback omits the actual clock shell"),
        ("scripts/run-mock-tests.sh", 'run_gate "Linux-container DOM and accessibility contract" 120 scripts/run-browser-tests-linux.sh',
         '# removed Linux browser execution', "must execute its Linux container"),
        ("scripts/run-mock-tests.sh", 'run_gate "browser launch policy" 120 python3 test/test_web_ui_browser_gate.py',
         '# removed browser policy test', "execute browser launch-policy tests"),
        ("test/web_ui_browser_gate.py", 'if sys.platform != "linux":',
         'if False:', "Linux-only browser launch guards"),
        ("scripts/run-mock-tests.sh", 'run_gate "locked cJSON intake and OOM" 120 bash test/run-cjson-oom-tests.sh',
         '# removed real cJSON intake tests', "locked cJSON intake/OOM"),
        ("scripts/run-mock-tests.sh", "need_tool cmake", "# removed", "cmake"),
        ("scripts/run-mock-tests.sh", "tools/agent-hooks/run_with_timeout.py", "missing.py", "bounded"),
        ("tools/agent-hooks/agent_hook.py", '[str(script), "--require-all"]', "[str(script)]", "strict"),
        ("test/CMakeLists.txt", "add_executable(runtime_boundary_tests", "add_executable(runtime_missing",
         "runtime_boundary_tests"),
        ("scripts/run-mock-tests.sh", "main/stack_watch.cpp", "main/missing_stack_watch.cpp",
         "fallback omits main/stack_watch.cpp"),
        ("test/CMakeLists.txt", "../main/stack_watch.cpp", "../main/missing_stack_watch.cpp",
         "actual stack minimum shell"),
        ("scripts/run-fast-tests.sh", "main/stack_watch.cpp", "main/missing_stack_watch.cpp",
         "actual stack minimum shell"),
        ("scripts/run-mock-tests.sh", "main/safe_mode.cpp", "main/missing_safe_mode.cpp",
         "fallback omits main/safe_mode.cpp"),
        ("scripts/run-mock-tests.sh", "python3 scripts/check-nvs-contract.py --self-test",
         "python3 scripts/missing-nvs-contract.py --self-test", "execute the NVS"),
        ("scripts/run-mock-tests.sh", "python3 scripts/check-dependency-contract.py --self-test",
         "python3 scripts/missing-dependency-contract.py --self-test", "dependency contract gate"),
        ("scripts/run-mock-tests.sh", 'run_gate "tesla-ble incremental source admission" 120 python3 test/test_patch_incremental.py',
         '# removed incremental patch admission', "does not execute incremental patch admission"),
        ("scripts/run-mock-tests.sh", "python3 scripts/check-otadata-contract.py --self-test",
         "python3 scripts/missing-otadata-contract.py --self-test", "otadata contract gate"),
        ("scripts/run-mock-tests.sh", "python3 scripts/prepare-reused-release.py --self-test",
         "python3 scripts/missing-reused-release.py --self-test", "immutable Release reuse gate"),
        ("scripts/run-mock-tests.sh", "        scripts/check-dependency-contract.py \\\n",
         "", "strict gate_file inventory"),
        ("scripts/run-mock-tests.sh", "        scripts/check-otadata-contract.py \\\n",
         "", "strict gate_file inventory"),
        ("scripts/run-mock-tests.sh", "        scripts/prepare-reused-release.py \\\n",
         "", "strict gate_file inventory"),
        ("scripts/run-mock-tests.sh",
         "python3 scripts/check-logic-test-ownership.py --compile --self-test",
         "python3 scripts/check-logic-test-ownership.py --self-test",
         "exact logic ownership/compile"),
        ("scripts/run-mock-tests.sh", "python3 scripts/check-host-gate-contract.py",
         "python3 scripts/missing-host-gate-contract.py", "host wiring contract"),
        ("test/CMakeLists.txt", "add_test(NAME nvs_storage_tests COMMAND nvs_storage_tests)",
         "# removed nvs CTest", "nvs_storage_tests is not part"),
        ("test/CMakeLists.txt", "test_nvs_storage.cpp", "missing_nvs_storage.cpp",
         "target/source wiring"),
        ("scripts/run-mock-tests.sh",
         'run_gate "execute NVS adapter tests" 120 "$BUILD_DIR/nvs_storage_tests"',
         "# removed NVS fallback execution", "compile and execute nvs_storage_tests"),
        ("test/web_ui_browser_gate.py", "PROFILE_TIMEOUT_SECONDS = 45",
         "PROFILE_TIMEOUT_SECONDS = 20", "cold-start and termination budgets"),
        ("scripts/run-mock-tests.sh",
         'run_gate "real-browser DOM and accessibility contract" 120 \\\n',
         'run_gate "real-browser DOM and accessibility contract" 90 \\\n',
         "120-second outer timeout"),
        ("scripts/ci-build-verify.sh", "CJSON_OOM_SANITIZE=1 bash ./test/run-cjson-oom-tests.sh",
         "true", "real-cJSON host gates"),
        ("scripts/ci-build-verify.sh", "MQTT_JSON_SANITIZE=1 bash ./test/run-mqtt-json-publish-tests.sh",
         "true", "real-cJSON host gates"),
    ]
    mutations.append(("scripts/run-sanitizer-tests.sh",
                      "python3 test/test_display_parity_contract.py --render-only --sanitize",
                      "true # removed native display renderer execution",
                      "actual display renderer sanitizer gate"))
    display_call = 'ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \\\n    LSAN_OPTIONS="$lsan_options" CXX="$CXX" python3 test/test_display_parity_contract.py --render-only --sanitize'
    mutations.extend([
        ("scripts/run-sanitizer-tests.sh", display_call, 'if false; then\n' + display_call + '\nfi',
         "sanitizer invocation flow"),
        ("scripts/run-sanitizer-tests.sh", display_call, 'exit 0\n' + display_call,
         "sanitizer invocation flow"),
    ])
    for relative, old, new, expected in mutations:
        with tempfile.TemporaryDirectory(prefix="host-gate-contract-") as directory:
            fixture = Path(directory)
            for path in (
                "scripts/run-mock-tests.sh",
                "scripts/ci-build-verify.sh",
                "scripts/test-build-contracts.sh",
                "scripts/run-sanitizer-tests.sh",
                "scripts/run-fast-tests.sh",
                "scripts/check-ble-row-parity.sh",
                "scripts/check-display-sim-parity.sh",
                "tools/agent-hooks/agent_hook.py",
                "test/CMakeLists.txt",
                "test/generate_effect_reply_adapters.py",
                "test/test_config_commit.cpp",
                "test/web_ui_browser_gate.py",
                "test/run-mqtt-json-publish-tests.sh",
                "test/test_mqtt_json_publish.cpp",
            ):
                destination = fixture / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(root / path, destination)
            replace_once(fixture / relative, old, new)
            try:
                validate(fixture)
            except ContractError as exc:
                require(expected in str(exc),
                        f"self-test mutation failed for wrong reason: {exc}")
            else:
                raise ContractError(f"self-test accepted mutation in {relative}: {old}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
        if args.self_test:
            self_test(args.root.resolve())
    except (OSError, UnicodeError, ContractError) as exc:
        print(f"host-gate-contract: {exc}", file=sys.stderr)
        return 1
    print("host-gate-contract: PASS" + (" (mutation canaries)" if args.self_test else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
