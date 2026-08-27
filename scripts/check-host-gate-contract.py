#!/usr/bin/env python3
"""Static and mutation-tested fail-closed contract for CI/Stop host gates."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import sys
import tempfile


class ContractError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def validate(root: Path) -> None:
    run_mock = (root / "scripts/run-mock-tests.sh").read_text(encoding="utf-8")
    idf_verify = (root / "scripts/ci-build-verify.sh").read_text(encoding="utf-8")
    hook = (root / "tools/agent-hooks/agent_hook.py").read_text(encoding="utf-8")
    cmake = (root / "test/CMakeLists.txt").read_text(encoding="utf-8")
    browser_gate = (root / "test/web_ui_browser_gate.py").read_text(encoding="utf-8")
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
        "scripts/check-bench-acceptance.py",
        "scripts/check-dependency-contract.py",
        "scripts/check-host-gate-contract.py",
        "scripts/check-logic-test-ownership.py",
        "scripts/check-nvs-contract.py",
        "scripts/check-otadata-contract.py",
        "scripts/prepare-reused-release.py",
        "scripts/run-fuzz-smoke.sh",
        "test/web_ui_browser_gate.py",
    ):
        require(path in run_mock, f"run-mock-tests.sh strict inventory omits {path}")
    require("python3 scripts/check-logic-test-ownership.py --compile --self-test" in run_mock,
            "run-mock-tests.sh does not execute the exact logic ownership/compile gate")
    require("python3 scripts/check-host-gate-contract.py" in run_mock,
            "run-mock-tests.sh does not execute the host wiring contract")
    require("python3 scripts/check-nvs-contract.py --self-test" in run_mock,
            "run-mock-tests.sh does not execute the NVS contract gate")
    require("python3 scripts/check-dependency-contract.py --self-test" in run_mock,
            "run-mock-tests.sh does not execute the dependency contract gate")
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
    require("add_executable(runtime_boundary_tests" in cmake,
            "runtime_boundary_tests target is missing from host CMake")
    require("add_test(NAME runtime_boundary_tests" in cmake,
            "runtime_boundary_tests is not part of ctest")
    require("add_executable(nvs_storage_tests" in cmake and
            "test_nvs_storage.cpp" in cmake and
            "../main/nvs_storage.cpp" in cmake and
            "../main/config_blob.cpp" in cmake,
            "nvs_storage_tests target/source wiring is incomplete")
    require("add_test(NAME nvs_storage_tests COMMAND nvs_storage_tests)" in cmake,
            "nvs_storage_tests is not part of ctest")
    for source in (
        "test/test_runtime_boundaries.cpp",
        "main/diag_log.cpp",
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
        "            python3 test/web_ui_browser_gate.py --require-browser" in run_mock,
        "strict real-browser gate must retain its 120-second outer timeout",
    )
    require(
        "PROFILE_TIMEOUT_SECONDS = 45" in browser_gate
        and browser_gate.count("process.wait(timeout=PROFILE_TIMEOUT_SECONDS)") == 1
        and "TERMINATION_GRACE_SECONDS = 2" in browser_gate
        and browser_gate.count("process.wait(timeout=TERMINATION_GRACE_SECONDS)") == 2,
        "real-browser gate cold-start and termination budgets drifted",
    )
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
        ("scripts/run-mock-tests.sh", "need_tool cmake", "# removed", "cmake"),
        ("scripts/run-mock-tests.sh", "tools/agent-hooks/run_with_timeout.py", "missing.py", "bounded"),
        ("tools/agent-hooks/agent_hook.py", '[str(script), "--require-all"]', "[str(script)]", "strict"),
        ("test/CMakeLists.txt", "add_executable(runtime_boundary_tests", "add_executable(runtime_missing",
         "runtime_boundary_tests"),
        ("scripts/run-mock-tests.sh", "main/safe_mode.cpp", "main/missing_safe_mode.cpp",
         "fallback omits main/safe_mode.cpp"),
        ("scripts/run-mock-tests.sh", "python3 scripts/check-nvs-contract.py --self-test",
         "python3 scripts/missing-nvs-contract.py --self-test", "execute the NVS"),
        ("scripts/run-mock-tests.sh", "python3 scripts/check-dependency-contract.py --self-test",
         "python3 scripts/missing-dependency-contract.py --self-test", "dependency contract gate"),
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
    for relative, old, new, expected in mutations:
        with tempfile.TemporaryDirectory(prefix="host-gate-contract-") as directory:
            fixture = Path(directory)
            for path in (
                "scripts/run-mock-tests.sh",
                "scripts/ci-build-verify.sh",
                "tools/agent-hooks/agent_hook.py",
                "test/CMakeLists.txt",
                "test/web_ui_browser_gate.py",
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
