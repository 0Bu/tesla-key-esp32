#!/usr/bin/env python3
"""Static and mutation-tested fail-closed contract for CI/Stop host gates."""

from __future__ import annotations

import argparse
from functools import lru_cache
from pathlib import Path
import shutil
import sys
import tempfile
import json
import os
import subprocess
import re
import shlex
import runpy

F137_WIRING_MODULES: dict[str, dict] = {}

DEPLOY_FLOW_VALIDATED: set[str] = set()


class ContractError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


@lru_cache(maxsize=8)
def probe_build_contract_modes(source: str) -> None:
    """Exercise the actual CLI/relevance prefix; no build or publication occurs."""
    boundary = '"$repo_root/scripts/select-release-version.sh" --self-test'
    require(source.count(boundary) == 1, "build-contract profile invocation flow boundary is missing")
    prefix = source.split(boundary, 1)[0]
    with tempfile.TemporaryDirectory(prefix="build-contract-modes-") as temporary:
        work = Path(temporary)
        scripts = work / "scripts"; scripts.mkdir()
        trace = work / "trace.bin"
        relevance = scripts / "release-relevance.sh"
        relevance.write_text("#!/bin/sh\nprintf '%s\\000' release-relevance \"$@\" '' >> \"$CONTRACT_MODE_TRACE\"\n[ \"$CONTRACT_MODE_FAIL\" != 1 ] || exit 37\n")
        relevance.chmod(0o700)
        script = scripts / "test-build-contracts.sh"
        script.write_text(prefix + 'printf \'%s\\000\' after \'\' >> "$CONTRACT_MODE_TRACE"\n')
        env = {k: v for k, v in os.environ.items() if k not in
               ("ENV", "BASH_ENV", "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "GH_TOKEN", "GITHUB_TOKEN")
               and not k.startswith(("BASH_FUNC_", "PYTHON", "LD_", "DYLD_"))}
        env["CONTRACT_MODE_TRACE"] = str(trace)
        call = ["release-relevance", "--self-test"]
        for arguments, fail, exit_code, expected in (
            ([], False, 0, [call, ["after"]]),
            ([], True, 37, [call]),
            (["--build-and-manifest"], False, 0, [["after"]]),
            (["--build-and-manifest"], True, 0, [["after"]]),
            (["--unknown"], False, 2, []),
            ([""], False, 2, []),
            (["positional"], False, 2, []),
            (["--build-and-manifest", "extra"], False, 2, []),
            (["--build-and-manifest", "--build-and-manifest"], False, 2, []),
        ):
            trace.unlink(missing_ok=True)
            try:
                result = subprocess.run(["/bin/bash", str(script), *arguments], cwd=work,
                                        env={**env, "CONTRACT_MODE_FAIL": str(int(fail))},
                                        capture_output=True, timeout=10)
            except subprocess.TimeoutExpired as error:
                raise ContractError("build-contract profile invocation flow timed out") from error
            calls = [[os.fsdecode(arg) for arg in block.split(b'\0')]
                     for block in trace.read_bytes().split(b'\0\0') if block] if trace.exists() else []
            require(result.returncode == exit_code and calls == expected,
                    "build-contract profile invocation flow must retain default coverage, exact mode and failures")


def probe_deploy_consumer_invocations(source: str) -> None:
    """Exercise the shipped shell prefix with inert tools and a failing consumer."""
    boundary = 'python3 "$repo_root/scripts/prepare-reused-release.py" --self-test'
    require(source.count(boundary) == 1, "signed deploy consumer flow boundary is missing")
    prefix = source.split(boundary, 1)[0]
    with tempfile.TemporaryDirectory(prefix="deploy-consumer-flow-") as temporary:
        work = Path(temporary).resolve()
        scripts, tools = work / "scripts", work / "tools"
        scripts.mkdir(); tools.mkdir()
        # Only this prefix is executed. Direct prerequisite shell commands are inert stubs;
        # Python prerequisites and the embedded entry-point fixture are intercepted below.
        for name in re.findall(r'^\s*"\$repo_root/(scripts/[A-Za-z0-9_.-]+)"', prefix, re.MULTILINE):
            file = work / name
            file.write_text("#!/bin/sh\nexit 0\n"); file.chmod(0o700)
        trace = work / "trace.bin"
        python = tools / "python3"
        python.write_text('''#!/bin/sh
printf '%s\\000' "$@" '' >> "$DEPLOY_FLOW_TRACE"
case "${1-}" in
  */test_deploy_candidate.py) if [ "$DEPLOY_FLOW_FAIL" = 1 ]; then exit 37; fi ;;
esac
''')
        python.chmod(0o700)
        script = scripts / "test-build-contracts.sh"
        script.write_text(prefix + 'python3 __deploy_flow_after__\n')
        env = {k: v for k, v in os.environ.items() if k not in
               ("ENV", "BASH_ENV", "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "GH_TOKEN", "GITHUB_TOKEN")
               and not k.startswith(("BASH_FUNC_", "PYTHON", "LD_", "DYLD_"))}
        env.update(PATH=str(tools) + os.pathsep + env.get("PATH", os.defpath),
                   DEPLOY_FLOW_TRACE=str(trace))
        for arguments, fail in (([], False), ([], True),
                                (["--build-and-manifest"], False), (["--build-and-manifest"], True)):
            if trace.exists(): trace.unlink()
            try:
                result = subprocess.run(["bash", str(script), *arguments], cwd=work,
                                        env={**env, "DEPLOY_FLOW_FAIL": str(int(fail))},
                                        capture_output=True, timeout=10)
            except subprocess.TimeoutExpired as error:
                raise ContractError("actual signed deploy consumer flow timed out") from error
            calls = [[os.fsdecode(arg) for arg in block.split(b'\0')]
                     for block in trace.read_bytes().split(b'\0\0') if block] if trace.exists() else []
            target = [str(work / "test/test_deploy_candidate.py")]
            relevant = [args for args in calls if args == target or args == ["__deploy_flow_after__"]]
            require(result.returncode == (37 if fail else 0) and
                    relevant == ([target] if fail else [target, ["__deploy_flow_after__"]]),
                    "actual signed deploy consumer must run once in order and propagate failure")


@lru_cache(maxsize=8)
def probe_sanitizer_invocations(source: str, watched_target: str = "heap_supervisor_tests") -> None:
    """Execute shell control flow with passive tools, not a second sanitizer run."""
    require(watched_target in ("heap_supervisor_tests", "stack_sample_tests"), "unknown sanitizer flow target")
    with tempfile.TemporaryDirectory(prefix="sanitizer-flow-") as directory:
        work = Path(directory)
        scripts, binaries, scratch = work / "scripts", work / "bin", work / "tmp"
        for path in (scripts, binaries, scratch):
            path.mkdir()
        script = scripts / "run-sanitizer-tests.sh"
        script.write_text(source + '\npython3 __identity_flow_after__\n')
        trace = work / "calls.jsonl"
        # The fake compiler/CMake only produce successful disposable executables. The
        # actual shell must still traverse its prerequisites, loops and two Python calls.
        heap_program = ("#!" + sys.executable + "\nimport json,os\n"
                        "with open(os.environ['TESLA_FLOW_TRACE'],'a') as out: "
                        "out.write(json.dumps(['__native__',os.environ['TESLA_FLOW_WATCHED_TARGET']])+'\\n')\n"
                        "raise SystemExit(37 if os.environ.get('TESLA_FLOW_FAIL_HEAP') == '1' else 0)\n")
        stub = '''#!PYTHON
import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
if name == "python3":
    with open(os.environ["TESLA_FLOW_TRACE"], "a") as out:
        out.write(json.dumps(args) + "\\n")
    if args == ["test/test_f137_host.py", "--source", ".", "--sanitize", "--execute-controlled-host-only"] and os.environ.get("F137_FLOW_FAIL") == "1": raise SystemExit(37)
    if args == ["test/test_identity_recovery_contract.py", "--sanitize"] and os.environ.get("TESLA_FLOW_FAIL_IDENTITY") == "1":
        raise SystemExit(37)
elif name == "cmake" and "-B" in args:
    build = Path(args[args.index("-B") + 1]); build.mkdir(parents=True, exist_ok=True)
    for target in ("logic_tests", "nvs_storage_tests", "runtime_boundary_tests", "clock_runtime_tests",
                   "config_commit_tests", "syslog_runtime_tests", "crash_runtime_xtensa_tests", "crash_runtime_riscv_tests", "heap_supervisor_tests", "stack_sample_tests"):
        output = build / target
        if target == os.environ["TESLA_FLOW_WATCHED_TARGET"]:
            output.write_text(HEAP_EXECUTABLE_PROGRAM)
        else:
            output.write_text("#!/bin/sh\\nexit 0\\n")
        output.chmod(0o700)
elif name == "compiler" and "-o" in args:
    output = Path(args[args.index("-o") + 1])
    if str(output) != "/dev/null":
        output.write_text("#!/bin/sh\\nexit 0\\n"); output.chmod(0o700)
'''.replace("#!PYTHON", "#!" + sys.executable).replace("HEAP_EXECUTABLE_PROGRAM", repr(heap_program))
        for name in ("cmake", "compiler", "python3"):
            path = binaries / name
            path.write_text(stub)
            path.chmod(0o700)
        uname = binaries / "uname"
        uname.write_text("#!/bin/sh\nprintf 'Linux\\n'\n")
        uname.chmod(0o700)
        # Passive actual-shell join: never execute the backend/compiler during flow tests.
        repair = work / "test/run-config-repair-backend.sh"
        repair.parent.mkdir()
        repair.write_text('#!/bin/sh\n[ "${CONFIG_REPAIR_SANITIZE-}" = 1 ] || exit 38\nprintf \'%s\\n\' \'["__repair_backend__"]\' >> "$TESLA_FLOW_TRACE"\n[ "${TESLA_FLOW_FAIL_REPAIR-0}" = 0 ] || exit 41\n')
        env = os.environ.copy()
        for name in list(env):
            if name in ("OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "ENV", "BASH_ENV") or name.startswith("BASH_FUNC_"):
                env.pop(name, None)
        env.update(PATH=str(binaries) + os.pathsep + env.get("PATH", os.defpath),
                   CC=str(binaries / "compiler"), CXX=str(binaries / "compiler"),
                   TMPDIR=str(scratch), TESLA_FLOW_TRACE=str(trace), TESLA_FLOW_WATCHED_TARGET=watched_target)
        for fail in ("none", "heap", "f137", "identity", "repair"):
            trace.unlink(missing_ok=True)
            try:
                result = subprocess.run(["/bin/bash", str(script)],
                                        env={**env, "TESLA_FLOW_FAIL_HEAP": str(int(fail == "heap")), "F137_FLOW_FAIL": str(int(fail == "f137")), "TESLA_FLOW_FAIL_IDENTITY": str(int(fail == "identity")), "TESLA_FLOW_FAIL_REPAIR": str(int(fail == "repair"))},
                                        cwd=work, capture_output=True, text=True, timeout=10)
            except subprocess.TimeoutExpired as exc:
                raise ContractError("sanitizer invocation flow timed out") from exc
            calls = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
            expected = [["__native__", watched_target]]
            if fail != "heap":
                expected += [["test/test_led_driver_contract.py", "--sanitize"],
                             ["test/test_display_parity_contract.py", "--render-only", "--sanitize"],
                             ["test/test_f137_host.py", "--source", ".", "--sanitize", "--execute-controlled-host-only"]]
                if fail != "f137":
                    expected += [["test/test_identity_recovery_contract.py", "--sanitize"]]
                    if fail != "identity":
                        expected += [["__repair_backend__"]]
                        if fail != "repair":
                            expected += [["__identity_flow_after__"]]
            expected_exit = 41 if fail == "repair" else 0 if fail == "none" else 37
            require(result.returncode == expected_exit and calls == expected,
                    "sanitizer invocation flow must execute the watched native target, F137, identity recovery and maintenance backend once and propagate failure before later probes")


def probe_heap_suite_invocations(run_mock: str, fast: str, watched_target: str = "heap_supervisor_tests") -> None:
    """Execute actual fallback/fast shell paths with inert compiler and executable effects."""
    branch = 'if command -v cmake >/dev/null 2>&1; then'
    boundary = '# Display sim'
    require(run_mock.count(branch) == 1 and run_mock.count(boundary) == 1,
            "Heap fallback flow seam is missing")
    # Select only the documented no-CMake branch. All guards, generators, compile argv and
    # executable invocations in that branch remain the actual shipped shell source.
    fallback = run_mock.split(boundary, 1)[0].replace(branch, 'if false; then', 1)
    probe_heap_suite_shells(fallback, fast, watched_target)


@lru_cache(maxsize=8)
def probe_heap_suite_shells(fallback: str, fast: str, watched_target: str = "heap_supervisor_tests") -> None:
    require(watched_target in ("heap_supervisor_tests", "stack_sample_tests"), "unknown native flow target")
    # Cache only the complete executed texts. Later unrelated mock gates cannot change an
    # earlier fallback effect; every change to that prefix or the full fast script reruns it.
    for label, source, arguments in (("fallback", fallback, []), ("fast", fast, ["--boundary"])):
        with tempfile.TemporaryDirectory(prefix="heap-host-flow-") as directory:
            work = Path(directory); scripts = work / "scripts"; tools = work / "bin"
            scripts.mkdir(); tools.mkdir(); trace = work / "trace.jsonl"
            script = scripts / ("run-mock-tests.sh" if label == "fallback" else "run-fast-tests.sh")
            script.write_text(source + '\npython3 __heap_flow_after__\n')
            stub = '''#!PYTHON
import json,os,sys
from pathlib import Path
def note(value):
    with open(os.environ['TESLA_HEAP_FLOW_TRACE'],'a') as out: out.write(json.dumps(value)+'\\n')
args=sys.argv[1:]
if Path(sys.argv[0]).name == 'python3':
    if args and args[0] == 'test/generate_heap_supervisor_adapter.py' and os.environ['TESLA_FLOW_WATCHED_TARGET']=='heap_supervisor_tests': note(['generate'])
    if args == ['__heap_flow_after__']: note(['after'])
elif '-o' in args:
    output=Path(args[args.index('-o')+1]); output.parent.mkdir(parents=True,exist_ok=True)
    if output.name == os.environ['TESLA_FLOW_WATCHED_TARGET']:
        required=({'test/test_heap_supervisor.cpp','main/safe_mode.cpp','main/heap_trend.cpp','main/stack_watch.cpp'} if os.environ['TESLA_FLOW_WATCHED_TARGET']=='heap_supervisor_tests' else {'test/test_stack_sample.cpp','-pthread'})
        if not required <= set(args): raise SystemExit(38)
        note(['compile'])
        program="#!"+sys.executable+"\\nimport json,os\\nwith open(os.environ['TESLA_HEAP_FLOW_TRACE'],'a') as out: out.write(json.dumps(['run'])+'\\\\n')\\nraise SystemExit(37 if os.environ['TESLA_HEAP_FLOW_FAIL']=='1' else 0)\\n"
    else: program='#!/bin/sh\\nexit 0\\n'
    output.write_text(program); output.chmod(0o700)
'''.replace("#!PYTHON", "#!" + sys.executable)
            for name in ("python3", "compiler"):
                tool = tools / name; tool.write_text(stub); tool.chmod(0o700)
            env = {k: v for k, v in os.environ.items() if k not in
                   ("ENV", "BASH_ENV", "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "GH_TOKEN", "GITHUB_TOKEN")
                   and not k.startswith(("BASH_FUNC_", "PYTHON", "LD_", "DYLD_"))}
            env.update(PATH=str(tools) + os.pathsep + os.defpath,
                       CXX=str(tools / "compiler"), CC=str(tools / "compiler"),
                       TESLA_HEAP_FLOW_TRACE=str(trace), TESLA_FLOW_WATCHED_TARGET=watched_target)
            for fail in (False, True):
                trace.unlink(missing_ok=True)
                try:
                    result = subprocess.run(["/bin/bash", str(script), *arguments], cwd=work,
                                            env={**env, "TESLA_HEAP_FLOW_FAIL": str(int(fail))},
                                            capture_output=True, timeout=10)
                except subprocess.TimeoutExpired as exc:
                    raise ContractError("Heap " + label + " invocation flow timed out") from exc
                calls = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
                expected = ([["generate"]] if watched_target == "heap_supervisor_tests" else []) + [["compile"], ["run"]] + ([] if fail else [["after"]])
                require(result.returncode == (37 if fail else 0) and calls == expected,
                        "Heap " + label + " invocation flow must generate, compile and execute once and propagate failure")


# These isolated passive fixtures depend only on the complete shell text. Reuse a successful
# result within this process; changed text reruns, and failures are never cached.
@lru_cache(maxsize=8)
def probe_browser_invocations(source: str) -> None:
    """Execute the actual strict shell flow with passive tools, without compiling or a browser."""
    inventory = re.search(r"for required in (.*?)\s*; do", source, re.DOTALL)
    require(inventory is not None, "strict host inventory loop is missing")
    required = shlex.split(inventory.group(1).replace("\\\n", " "))
    for platform in ("Linux", "Darwin"):
        with tempfile.TemporaryDirectory(prefix="browser-host-flow-") as directory:
            work = Path(directory)
            for name in [*required, "tools/agent-hooks/run_with_timeout.py"]:
                relative = Path(name)
                require(not relative.is_absolute() and ".." not in relative.parts,
                        "unsafe strict host fixture path")
                file = work / relative
                file.parent.mkdir(parents=True, exist_ok=True)
                file.touch()
            script = work / "scripts/run-mock-tests.sh"
            script.write_text(source)
            binaries = work / "bin"
            binaries.mkdir()
            trace = work / "trace.jsonl"
            python = binaries / "python3"
            python.write_text("#!" + sys.executable + '''
import json,os,sys
if sys.argv[1:2] != ['-c']:
    with open(os.environ['TESLA_BROWSER_FLOW_TRACE'],'a') as out:
        out.write(json.dumps(sys.argv[1:])+'\\n')
    if sys.argv[1:] == json.loads(os.environ.get('TESLA_BROWSER_FLOW_FAIL','null')):
        raise SystemExit(37)
''')
            python.chmod(0o700)
            for name in ("cmake", "node"):
                file = binaries / name
                file.write_text("#!/bin/sh\nexit 0\n")
                file.chmod(0o700)
            uname = binaries / "uname"
            uname.write_text("#!/bin/sh\nprintf '%s\\n' " + shlex.quote(platform) + "\n")
            uname.chmod(0o700)
            env = os.environ.copy()
            for name in tuple(env):
                if name in ("OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "GH_TOKEN", "GITHUB_TOKEN",
                            "ENV", "BASH_ENV") or name.startswith(("PYTHON", "BASH_FUNC_")):
                    env.pop(name, None)
            env.update(PATH=str(binaries) + os.pathsep + os.defpath,
                       TESLA_BROWSER_FLOW_TRACE=str(trace))
            prefix = ["tools/agent-hooks/run_with_timeout.py", "120"]
            expected = [prefix + ["python3", "test/test_f137_host.py", "--source", ".", "--controls", "--execute-controlled-host-only"],
                        prefix + ["scripts/release-relevance.sh", "--self-test"],
                        prefix + ["scripts/test-build-contracts.sh", "--build-and-manifest"],
                        prefix + ["python3", "test/test_dependency_sources.py"],
                        prefix + ["python3", "test/test_dependency_cmake.py"],
                        prefix + ["python3", "test/test_dependency_harness.py"],
                        prefix + ["python3", "test/test_dependency_process.py"],
                        prefix + ["python3", "test/test_identity_recovery_contract.py"],
                        prefix + ["python3", "test/test_inline_assets.py"],
                        prefix + ["python3", "test/test_web_ui_browser_gate.py"],
                        prefix + ["python3", "test/test_browser_cdp.py"],
                        prefix + (["scripts/run-browser-tests-linux.sh"] if platform == "Darwin" else
                                  ["python3", "test/web_ui_browser_gate.py", "--require-browser"])]
            for failure in [None, *expected]:
                trace.unlink(missing_ok=True)
                env["TESLA_BROWSER_FLOW_FAIL"] = json.dumps(failure)
                try:
                    result = subprocess.run(["/bin/bash", str(script), "--require-all"], cwd=work,
                                            env=env, capture_output=True, timeout=10)
                except subprocess.TimeoutExpired as error:
                    raise ContractError("strict browser invocation flow timed out") from error
                calls = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
                if failure is None:
                    relevance_call = prefix + ["scripts/release-relevance.sh", "--self-test"]
                    build_call = prefix + ["scripts/test-build-contracts.sh", "--build-and-manifest"]
                    require(result.returncode == 0 and all(calls.count(call) == 1 for call in expected)
                            and calls.index(relevance_call) < calls.index(build_call),
                            "strict browser invocation flow must execute policy, pipe and real-browser gates once")
                else:
                    require(result.returncode == 1 and calls.count(failure) == 1,
                            "strict browser invocation flow must propagate each gate failure")


def probe_stack_hook_invocations(source: str) -> None:
    """Run the actual run_gate/Python branch with inert tools, never compile or sample RTOS."""
    label = "stack hook invocation flow"
    gate = re.findall(r"(?ms)^run_gate\(\) \{\n.*?^\}", source)
    begin, end = "# Display sim", '    run_gate "runtime C-boundary inventory"'
    require(len(gate) == 1 and source.count(begin) == source.count(end) == 1,
            label + ": actual shell seams are missing/ambiguous")
    mode = re.findall(r"(?m)^set -euo pipefail$", source)
    timeout = re.findall(r"(?m)^TIMEOUT_RUNNER=tools/agent-hooks/run_with_timeout.py$", source)
    require(len(mode) == len(timeout) == 1, label + ": actual shell mode/timeout runner changed")
    branch = source[source.index(begin):source.index(end)]
    program = (mode[0] + "\n" + timeout[0] + "\n" + gate[0] + "\n" + branch +
               "    python3 __stack_hook_flow_after__\nfi\n")
    probe_stack_hook_program(program)


@lru_cache(maxsize=8)
def probe_stack_hook_program(program: str) -> None:
    """Cache only an already successful identical complete extracted shell program."""
    label = "stack hook invocation flow"
    with tempfile.TemporaryDirectory(prefix="stack-hook-host-flow-") as directory:
        work = Path(directory); tools = work / "bin"; scripts = work / "scripts"
        tools.mkdir(); scripts.mkdir(); trace = work / "trace.jsonl"
        # Earlier gates in this exact branch are inert. Their own canonical probes remain
        # independently required by validate(); do not compile or launch their real effects here.
        parity = scripts / "check-display-sim-parity.sh"
        parity.write_text("#!/bin/sh\nexit 0\n"); parity.chmod(0o700)
        stub = '''#!PYTHON
import json, os, sys
args = sys.argv[1:]
watched = "test/test_stack_context_hooks.py"
def note(value):
    with open(os.environ["TESLA_STACK_HOOK_TRACE"], "a") as out:
        out.write(json.dumps(value) + "\\n")
if watched in args:
    note(["gate", args])
    expected = (["tools/agent-hooks/run_with_timeout.py", "120", "python3", watched, ".", "--self-test"]
                if os.environ["REQUIRE_ALL"] == "1" else [watched, ".", "--self-test"])
    if args != expected: raise SystemExit(38)
    raise SystemExit(37 if os.environ["TESLA_STACK_HOOK_FAIL"] == "1" else 0)
if args == ["__stack_hook_flow_after__"]: note(["after"])
'''.replace("#!PYTHON", "#!" + sys.executable)
        python = tools / "python3"; python.write_text(stub); python.chmod(0o700)
        script = scripts / "stack-hook-flow.sh"; script.write_text(program)
        env = {k: v for k, v in os.environ.items() if k not in
               ("ENV", "BASH_ENV", "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "GH_TOKEN", "GITHUB_TOKEN")
               and not k.startswith(("BASH_FUNC_", "PYTHON", "LD_", "DYLD_"))}
        env.update(PATH=str(tools) + os.pathsep + os.defpath, TESLA_STACK_HOOK_TRACE=str(trace))
        for strict in (False, True):
            args = (["tools/agent-hooks/run_with_timeout.py", "120", "python3",
                     "test/test_stack_context_hooks.py", ".", "--self-test"] if strict else
                    ["test/test_stack_context_hooks.py", ".", "--self-test"])
            for fail in (False, True):
                trace.unlink(missing_ok=True)
                try:
                    result = subprocess.run(["/bin/bash", str(script)], cwd=work,
                        env={**env, "REQUIRE_ALL": str(int(strict)),
                             "TESLA_STACK_HOOK_FAIL": str(int(fail))},
                        capture_output=True, timeout=10)
                except subprocess.TimeoutExpired as exc:
                    raise ContractError(label + ": passive fixture timed out") from exc
                calls = [json.loads(line) for line in trace.read_text().splitlines()] if trace.exists() else []
                expected = [["gate", args]] + ([] if fail else [["after"]])
                require(result.returncode == ((1 if strict else 37) if fail else 0) and calls == expected,
                        label + ": execute once with exact arguments/bound and propagate failure before continuation")


def native_compile_arguments(source: str, binary: str) -> list[str]:
    """Read one actual direct compile command, not a token elsewhere in the shell."""
    logical = source.replace("\\\n", " ")
    matches = []
    for line in logical.splitlines():
        if line.lstrip().startswith("#") or "-o" not in line or binary not in line:
            continue
        args = shlex.split(line)
        if "-o" in args and args.index("-o") + 1 < len(args) and args[args.index("-o") + 1] == binary:
            matches.append(args)
    require(len(matches) == 1, "F135 actual native compile command missing/ambiguous: " + binary)
    return matches[0]


def require_native_stack_joins(root: Path, cmake_code: str, run_mock: str, fast: str) -> None:
    """Every actual generated hook consumer links the real sampler; only HWM I/O is doubled."""
    for adapter in ("heap_supervisor_adapter.generated.hpp", "clock_adapters.generated.hpp",
                    "config_commit_runtime.generated.hpp"):
        marker = "OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/" + adapter
        require(cmake_code.count(marker) == 1, "F135 actual adapter output/dependencies: " + adapter)
        block = cmake_code[cmake_code.index(marker):]
        block = block[:block.index("VERBATIM)")]
        deps = re.search(r"DEPENDS\s+(.*)", block, re.DOTALL)
        require(deps is not None and {"../main/stack_watch.hpp", "../main/logic/stack_context.hpp",
                                    "../main/logic/stack_sample_codec.hpp"} <= set(deps[1].split()),
                "F135 actual adapter sampler dependencies: " + adapter)
    context_map = {
        "test/test_heap_supervisor.cpp": ("heap_supervisor_tests", ("HeapSupervisor", "Main"), 2048),
        "test/test_clock_runtime.cpp": ("clock_runtime_tests", ("SntpSync",), 4096),
        "test/test_config_commit.cpp": ("config_commit_tests", ("OtaGate", "CaptiveHttp"), 4096),
        "test/test_effect_reply_adapters.cpp": ("test_effect_reply_adapters", ("Httpd",), 4096),
    }
    for path, (target, lanes, amount) in context_map.items():
        source = (root / path).read_text()
        require('#include "stack_watch.hpp"' in source and
                not re.search(r"(?:enum\s+class\s+StackWatch\b|\bstack_watch_sample\s*\([^;{}]*\)\s*(?:noexcept\s*)?\{)", source),
                "F135 actual declaration/no-noop contract: " + path)
        spy = (r'extern\s+"C"\s+(?:unsigned|UBaseType_t)\s+uxTaskGetStackHighWaterMark\s*'
               r'\(\s*(?:TaskHandle_t|void\*)\s+task\s*\)\s*\{\s*CHECK\(task == nullptr\);\s*'
               r'\+\+f135_hwm_queries;\s*return\s+' + str(amount) + r';\s*\}')
        require(len(re.findall(spy, source)) == 1 and 'CHECK(f135_hwm_queries > 0);' in source,
                "F135 own-task HWM spy/actual hook oracle: " + path)
        for lane in lanes:
            require('CHECK(!tk::stack_watch_min_free_bytes(tk::StackWatch::' + lane + ').has_value());' in source and
                    'CHECK(tk::stack_watch_min_free_bytes(tk::StackWatch::' + lane + ') == ' + str(amount) + 'u);' in source,
                    "F135 actual generated hook lane oracle: " + path + ":" + lane)
        if target != "test_effect_reply_adapters":
            groups = re.findall(r"\badd_executable\(" + re.escape(target) + r"\s+([^)]*)\)", cmake_code)
            require(len(groups) == 1 and "../main/stack_watch.cpp" in groups[0].split(),
                    "F135 CMake real sampler join: " + target)
            includes = re.findall(r"\btarget_include_directories\(" + re.escape(target) + r"\s+PRIVATE\s+([^)]*)\)", cmake_code)
            require(len(includes) == 1 and "${CMAKE_CURRENT_SOURCE_DIR}/stubs" in includes[0].split(),
                    "F135 CMake SDK stub include join: " + target)
            args = native_compile_arguments(run_mock, "$BUILD_DIR/" + target)
            require("main/stack_watch.cpp" in args and "-Itest/stubs" in args,
                    "F135 fallback real sampler join: " + target)
    fast_args = native_compile_arguments(fast, "$BUILD_DIR/heap_supervisor_tests")
    require("main/stack_watch.cpp" in fast_args and "-Itest/stubs" in fast_args,
            "F135 fast real sampler join: heap_supervisor_tests")
    cjson = (root / "test/run-cjson-oom-tests.sh").read_text()
    args = native_compile_arguments(cjson, "$work/test_effect_reply_adapters")
    require("$repo_root/main/stack_watch.cpp" in args and "-I$repo_root/test/stubs" in args and
            '${sanitize_flags[@]+"${sanitize_flags[@]}"}' in cjson,
            "F135 cJSON real sampler/instrumented SDK stub join")


def validate(root: Path) -> None:
    run_mock = (root / "scripts/run-mock-tests.sh").read_text(encoding="utf-8")
    idf_verify = (root / "scripts/ci-build-verify.sh").read_text(encoding="utf-8")
    hook = (root / "tools/agent-hooks/agent_hook.py").read_text(encoding="utf-8")
    cmake = (root / "test/CMakeLists.txt").read_text(encoding="utf-8")
    sanitizers = (root / "scripts/run-sanitizer-tests.sh").read_text(encoding="utf-8")
    browser_gate = (root / "test/web_ui_browser_gate.py").read_text(encoding="utf-8")
    browser_cdp = (root / "test/browser_cdp.py").read_text(encoding="utf-8")
    fast = (root / "scripts/run-fast-tests.sh").read_text(encoding="utf-8")
    mqtt_gate_source = (root / "test/run-mqtt-json-publish-tests.sh").read_text(encoding="utf-8")
    mqtt_test = (root / "test/test_mqtt_json_publish.cpp").read_text(encoding="utf-8")
    require('python3 "$repo_root/test/generate_mqtt_discovery_adapter.py" "$work/mqtt_discovery_adapter.generated.hpp"' in mqtt_gate_source and
            '-I"$work"' in mqtt_gate_source and
            '#include "mqtt_discovery_adapter.generated.hpp"' in mqtt_test and
            '    test_current_discovery_address();' in mqtt_test,
            "actual MQTT discovery address fixture is missing from the retained JSON gate")
    publication_gate = (root / "scripts/test-build-contracts.sh").read_text()
    relevance_gate = 'run_gate "release relevance and publication selectors" 120 scripts/release-relevance.sh --self-test'
    build_gate = 'run_gate "build and manifest contract tests" 120 scripts/test-build-contracts.sh --build-and-manifest'
    require(run_mock.count(relevance_gate) == 1 and run_mock.count(build_gate) == 1,
            "strict browser invocation flow: release/build profile gates or exact arguments are missing")
    probe_build_contract_modes(publication_gate)
    require('        test/test_deploy_candidate.py \\\n' in run_mock,
            "actual signed deploy consumer tests are missing from the mandatory host gate")
    # Other mutation cases retain this exact shell body. Cache only an already executed
    # complete input, never a path/name; every changed shell must exercise the real flow.
    if publication_gate not in DEPLOY_FLOW_VALIDATED:
        probe_deploy_consumer_invocations(publication_gate)
        DEPLOY_FLOW_VALIDATED.add(publication_gate)
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
    require('run_gate "cached identity recovery and actual restart owner" 120 python3 test/test_identity_recovery_contract.py' in run_mock and
            'python3 test/test_identity_recovery_contract.py --sanitize' in sanitizers,
            "actual identity recovery host/sanitizer gate is missing")
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
    inventory = re.search(r"for required in (.*?)\s*; do", run_mock, re.DOTALL)
    require(inventory is not None, "strict host inventory loop is missing")
    strict_inputs = set(shlex.split(inventory.group(1).replace("\\\n", " ")))
    for path in (
        "test/tesla_protocol_vectors.test.mjs",
        "test/test_runtime_boundary_contract.py",
        "test/test_stack_context_hooks.py",
        "test/stack_context_hook_owners.json",
        "test/generate_identity_recovery_adapter.py",
        "test/test_identity_recovery.cpp",
        "test/test_identity_recovery_contract.py",
        "test/identity_recovery_process.py",
        "test/identity_recovery_omission_cases.json",
        "test/generate_heap_supervisor_adapter.py",
        "test/test_heap_supervisor.cpp",
        "test/stubs/esp_attr.h",
        "main/stack_watch.hpp",
        "main/stack_watch.cpp",
        "main/logic/stack_context.hpp",
        "main/logic/stack_sample_codec.hpp",
        "test/test_charge_presentation.py",
        "test/test_command_outcomes.py",
        "test/test_inline_assets.py",
        "main/www/prune_css_comments.py",
        "test/run-cjson-oom-tests.sh",
        "test/ota_operation_contract.py",
        "test/generate_ota_tls_adapter.py",
        "test/test_ota_tls_admission.cpp",
        "test/ota_http_parser/provenance.json",
        "test/ota_http_parser/http_parser.c",
        "test/ota_http_parser/http_parser.h",
        "test/test_codex_setup.py",
        "scripts/check-bench-acceptance.py",
        "scripts/check-dependency-contract.py",
        "scripts/check-bootloader-compat.py",
        "scripts/bootloader-references.json",
        "test/test_bootloader_preflight.py",
        "test/test_patch_incremental.py",
        "test/test_dependency_sources.py",
        "test/test_dependency_cmake.py",
        "test/test_dependency_harness.py",
        "test/test_dependency_process.py",
        "scripts/dependency_source.py",
        "scripts/nanopb-source.json",
        "cmake/admit-dependency-sources.cmake",
        "test/test_dependency_guards.py",
        "scripts/release-relevance.sh",
        "scripts/test-build-contracts.sh",
        "scripts/check-host-gate-contract.py",
        "scripts/check-logic-test-ownership.py",
        "scripts/check-nvs-contract.py",
        "scripts/check-otadata-contract.py",
        "scripts/prepare-reused-release.py",
        "scripts/run-fuzz-smoke.sh",
        "test/web_ui_browser_gate.py",
        "test/test_web_ui_browser_gate.py",
        "test/test_browser_cdp.py",
        "test/browser_cdp.py",
    ):
        require(path in strict_inputs, f"run-mock-tests.sh strict inventory omits {path}")
    require("python3 scripts/check-logic-test-ownership.py --compile --self-test" in run_mock,
            "run-mock-tests.sh does not execute the exact logic ownership/compile gate")
    require('run_gate "locked cJSON intake and OOM" 120 bash test/run-cjson-oom-tests.sh' in run_mock,
            "strict host gate must execute the locked cJSON intake/OOM tests")
    require("python3 scripts/check-host-gate-contract.py" in run_mock,
            "run-mock-tests.sh does not execute the host wiring contract")
    require('run_gate "coherent charge presentation snapshot" 120 python3 test/test_charge_presentation.py' in run_mock,
            "strict host gate must execute the coherent charge presentation snapshot")
    require('run_gate "contextual charging command outcomes" 120 python3 test/test_command_outcomes.py' in run_mock,
            "strict host gate must execute contextual charging command outcomes")
    require('run_gate "CSS bytes and actual inline generation" 120 python3 test/test_inline_assets.py' in run_mock,
            "strict host gate must execute actual CSS inline generation")
    require('run_gate "retained bootloader compatibility and USB consumers" 120 python3 test/test_bootloader_preflight.py' in run_mock,
            "strict host gate must execute retained-bootloader and USB-consumer admission controls")
    require("python3 scripts/check-nvs-contract.py --self-test" in run_mock,
            "run-mock-tests.sh does not execute the NVS contract gate")
    require("python3 scripts/check-dependency-contract.py --self-test" in run_mock,
            "run-mock-tests.sh does not execute the dependency contract gate")
    require('run_gate "tesla-ble incremental source admission" 120 python3 test/test_patch_incremental.py' in run_mock,
            "run-mock-tests.sh does not execute incremental patch admission")
    require('run_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py' in run_mock,
            "run-mock-tests.sh does not execute exact dependency source admission")
    require('run_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py' in run_mock,
            "run-mock-tests.sh does not execute dependency source build ordering")
    require('run_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py' in run_mock,
            "run-mock-tests.sh does not execute dependency harness source ordering")
    require('run_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py' in run_mock,
            "run-mock-tests.sh does not execute dependency query process lifetime")
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
    require('include(${CMAKE_CURRENT_SOURCE_DIR}/config_repair_backend.cmake)' in cmake,
            'actual maintenance backend CMake graph missing')
    require('run_gate "maintenance actual backend source contract" 120 python3 scripts/check-config-repair-backend-contract.py' in run_mock and
            'run_gate "maintenance actual backend and console" 120 bash test/run-config-repair-backend.sh' in run_mock,
            'actual maintenance backend host entry missing')
    require('CONFIG_REPAIR_SANITIZE=1 bash test/run-config-repair-backend.sh' in sanitizers,
            'actual maintenance backend sanitizer entry missing')
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
    ota_native = (root / "test/run-cjson-oom-tests.sh").read_text()
    for token in ("main/ota_transport.cpp", "main/ota_http.cpp", "main/ota_coordinator.cpp",
                  "main/ota_writer.cpp", "main/stack_watch.cpp", "test/ota_http_parser/http_parser.c",
                  '"$work/test_ota_tls_admission" normal', '"$work/test_ota_tls_admission" commit',
                  '"$work/test_ota_tls_admission" uncertain', '-DF145_HOST_TEST_HOOKS=1',
                  '"$work/test_ota_tls_admission" budget', '"$work/test_ota_tls_admission" cleanup-cas',
                  '"$work/test_ota_tls_admission" cleanup-failed'):
        require(token in ota_native, "actual OTA native family missing " + token)
    require('ota_operation_contract.py' in (root / 'test/test_runtime_boundary_contract.py').read_text(),
            'actual OTA closed source contract is not called by the runtime boundary gate')
    require("add_test(NAME runtime_boundary_tests" in cmake,
            "runtime_boundary_tests is not part of ctest")
    # The Heap suite must be the first active Python-generated group, not a retained token in
    # a disabled nested block. Keep its entire source/generator graph and CTest admission bound.
    # CMake bracket comments may contain an entire otherwise valid target group. Remove the
    # complete comment, including matching optional '=' delimiters, before line comments.
    cmake_code = re.sub(r"#\[(=*)\[.*?\]\1\]",
                        lambda match: "\n" * match.group(0).count("\n"), cmake, flags=re.DOTALL)
    cmake_code = re.sub(r"(?m)#.*$", "", cmake_code)
    heap_at = cmake_code.find("find_package(Python3 COMPONENTS Interpreter QUIET)")
    require(heap_at >= 0 and not re.search(
        r"\b(?:if|elseif|else|function|macro|return|foreach|while)\s*\(", cmake_code[:heap_at]),
        "Heap CMake group must not be hidden behind an earlier conditional or return")
    first = cmake_code.find("add_custom_command(", heap_at)
    following = cmake_code.find("add_custom_command(", first + 1)
    require(first >= heap_at and following > first, "Heap CMake generator group is missing")
    heap_group = cmake_code[heap_at:following]
    require(re.findall(r"\b(?:if|elseif|else|function|macro|return|foreach|while)\s*\(", heap_group) == ["if("],
            "Heap CMake group must have only its Python-availability condition")
    require("if(Python3_Interpreter_FOUND)" in heap_group and
            "OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/heap_supervisor_adapter.generated.hpp" in heap_group and
            "COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_heap_supervisor_adapter.py" in heap_group and
            "add_executable(heap_supervisor_tests test_heap_supervisor.cpp" in heap_group and
            "${CMAKE_CURRENT_BINARY_DIR}/heap_supervisor_adapter.generated.hpp" in heap_group and
            "../main/safe_mode.cpp ../main/heap_trend.cpp" in heap_group and
            "add_test(NAME heap_supervisor_tests COMMAND heap_supervisor_tests)" in heap_group,
            "Heap CMake generator, real sources or CTest admission is incomplete")
    dependencies = re.search(r"DEPENDS\s+(.*?)\s+VERBATIM\)", heap_group, re.DOTALL)
    require(dependencies is not None and set(dependencies.group(1).split()) == {
        "generate_heap_supervisor_adapter.py", "test_runtime_boundary_contract.py",
        "../main/heap_supervisor.cpp", "../main/runtime_admission.cpp", "../main/ota_update.cpp",
        "../main/main.cpp", "../main/vehicle_telemetry.cpp", "../main/vehicle_ctrl.cpp",
        "../main/stack_watch.hpp", "../main/logic/stack_context.hpp", "../main/logic/stack_sample_codec.hpp"},
        "Heap CMake actual-body generator dependencies are incomplete")
    for entry, label in ((run_mock, "fallback"), (fast, "fast")):
        require('python3 test/generate_heap_supervisor_adapter.py' in entry and
                'test/test_heap_supervisor.cpp main/safe_mode.cpp main/heap_trend.cpp' in entry and
                '"$BUILD_DIR/heap_supervisor_tests"' in entry,
                "Heap " + label + " source generation/compile/execution is incomplete")
    stack_group = """add_executable(stack_sample_tests test_stack_sample.cpp)
target_include_directories(stack_sample_tests PRIVATE stubs ../main)
target_link_libraries(stack_sample_tests PRIVATE Threads::Threads)
add_test(NAME stack_sample_tests COMMAND stack_sample_tests)"""
    require(stack_group in cmake_code[:heap_at] and
            '#include "../main/stack_watch.cpp"' in (root / "test/test_stack_sample.cpp").read_text(),
            "actual Stack sample CMake/source group is incomplete or conditional")
    require('test/test_stack_sample.cpp' in fast and '-pthread' in fast and
            'run_gate "compile stack sample tests" 120 "$CXX"' in run_mock and
            'run_gate "execute stack sample tests" 120 "$BUILD_DIR/stack_sample_tests"' in run_mock,
            "actual Stack sample fast/fallback sources are incomplete")
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
    require('reference_body = body_in(reference_source, reference_name)' in config_generator and
            'reference_name = "ordinary_recovery_reference_ready_owned"' in config_generator and
            'reference = reference_source[reference_match.start():reference_open + len(reference_body)]' in config_generator and
            '    reference_policy_tests();' in config_test,
            "actual owned recovery-reference extraction or config execution is missing")
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
            '--target logic_tests nvs_storage_tests runtime_boundary_tests clock_runtime_tests config_commit_tests syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests heap_supervisor_tests stack_sample_tests' in sanitizers and
            'for target in logic_tests nvs_storage_tests runtime_boundary_tests clock_runtime_tests config_commit_tests syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests heap_supervisor_tests stack_sample_tests; do' in sanitizers,
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
    runtime_sources = re.findall(r"\badd_executable\(runtime_boundary_tests\s+([^)]*)\)", cmake_code)
    require(len(runtime_sources) == 1 and "../main/stack_watch.cpp" in runtime_sources[0].split() and
            "main/stack_watch.cpp" in native_compile_arguments(fast, "$BUILD_DIR/runtime_boundary_tests"),
            "actual stack minimum shell is missing from CMake/fast runtime tests")
    runtime_fallback = re.search(
        r'^\s*run_gate "compile runtime boundary tests" 120 (.*?)'
        r'(?=^\s*run_gate "execute runtime boundary tests")',
        run_mock, re.MULTILINE | re.DOTALL)
    require(runtime_fallback is not None,
            "cmake-less runtime_boundary_tests fallback compile command is missing")
    runtime_arguments = shlex.split(runtime_fallback.group(1).replace("\\\n", " "))
    for source in (
        "test/test_runtime_boundaries.cpp",
        "main/diag_log.cpp",
        "main/stack_watch.cpp",
        "main/safe_mode.cpp",
    ):
        require(source in runtime_arguments,
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
    require("BROWSER_WORK_SECONDS = 110" in browser_gate and "STARTUP_SECONDS = 45" in browser_gate
            and "time.monotonic() + BROWSER_WORK_SECONDS" in browser_gate
            and browser_cdp.count("process.wait(timeout=0.75)") == 2,
            "real-browser absolute work/startup and termination budgets drifted")
    require('("mobile", 390, 844, "light")' in browser_gate and
            '("tablet", 768, 900, "light")' in browser_gate and
            '("desktop", 1200, 900, "dark")' in browser_gate and
            'require(completed == [390, 768, 1200]' in browser_gate,
            "three populated browser profiles must complete")
    require('"Input.dispatchKeyEvent"' in browser_gate and '"trusted-" + code' in browser_gate and
            'ring["active"] and ring["visible"]' in browser_gate and
            'value["actions"] == index and len(value["clicks"]) == index' in browser_gate,
            "trusted native keyboard/focus acceptance is missing")
    require('run_gate "browser launch policy" 120 python3 test/test_web_ui_browser_gate.py' in run_mock,
            "host suite must execute browser launch-policy tests")
    require('run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py' in run_mock,
            "host suite must execute browser pipe-transport tests")
    require('if [ "$(uname -s)" = Darwin ]; then' in run_mock and
            'run_gate "Linux-container DOM and accessibility contract" 120 scripts/run-browser-tests-linux.sh' in run_mock,
            "strict macOS browser gate must execute its Linux container")
    require(browser_gate.count('if sys.platform != "linux":') == 4 and
            browser_cdp.count('if sys.platform != "linux":') == 1,
            "Linux-only browser launch guards drifted")
    cjson_gate = "CJSON_OOM_SANITIZE=1 bash ./test/run-cjson-oom-tests.sh"
    mqtt_gate = "MQTT_JSON_SANITIZE=1 bash ./test/run-mqtt-json-publish-tests.sh"
    build = './scripts/ci-build-all.sh "$version" "$source_sha"'
    require(cjson_gate in idf_verify and mqtt_gate in idf_verify,
            "pinned IDF orchestration omits sanitized real-cJSON host gates")
    require(idf_verify.index(cjson_gate) < idf_verify.index(build) and
            idf_verify.index(mqtt_gate) < idf_verify.index(build),
            "real-cJSON host gates must run before expensive firmware builds")
    # Reject structural omissions before the slower passive control-flow fixtures. Each fixture
    # still binds complete executed source text and runs all success/failure effects when needed.
    require_native_stack_joins(root, cmake_code, run_mock, fast)
    probe_stack_hook_invocations(run_mock)
    probe_sanitizer_invocations(sanitizers)
    probe_heap_suite_invocations(run_mock, fast)
    probe_heap_suite_invocations(run_mock, fast, "stack_sample_tests")
    probe_sanitizer_invocations(sanitizers, "stack_sample_tests")
    f137_helper = root / "test/test_f137_host_wiring.py"
    require(f137_helper.is_file(), "F137 host wiring helper missing")
    f137_text = f137_helper.read_text()
    if f137_text not in F137_WIRING_MODULES:
        F137_WIRING_MODULES[f137_text] = runpy.run_path(str(f137_helper))
    try:
        F137_WIRING_MODULES[f137_text]["validate"](root)
    except ValueError as error:
        raise ContractError(str(error)) from error
    probe_browser_invocations(run_mock)


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    require(old in text, f"self-test fixture text is absent: {old}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


IDENTITY_SANITIZER_COMMAND_BLOCK = 'ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS="$ubsan_options" \\\n    LSAN_OPTIONS="$lsan_options" CXX="$CXX" python3 test/test_identity_recovery_contract.py --sanitize\n'


def self_test(root: Path) -> None:
    validate(root)
    mutations = [
        ("scripts/run-mock-tests.sh", 'run_gate "cached identity recovery and actual restart owner" 120 python3 test/test_identity_recovery_contract.py',
         '# removed actual recovery gate', "actual identity recovery host/sanitizer gate is missing"),
        ("scripts/run-sanitizer-tests.sh", 'python3 test/test_identity_recovery_contract.py --sanitize',
         'true # removed actual recovery sanitizers', "actual identity recovery host/sanitizer gate is missing"),
        ("scripts/run-sanitizer-tests.sh", 'python3 test/test_identity_recovery_contract.py --sanitize',
         'python3 test/test_identity_recovery_contract.py --sanitize || true', "sanitizer invocation flow"),
        ("scripts/run-sanitizer-tests.sh", IDENTITY_SANITIZER_COMMAND_BLOCK,
         "if false; then\n" + IDENTITY_SANITIZER_COMMAND_BLOCK + "fi\n", "sanitizer invocation flow"),
        ("test/CMakeLists.txt", 'include(${CMAKE_CURRENT_SOURCE_DIR}/config_repair_backend.cmake)',
         '# omitted maintenance backend graph', 'actual maintenance backend CMake graph missing'),
        ("scripts/run-mock-tests.sh", 'run_gate "maintenance actual backend and console" 120 bash test/run-config-repair-backend.sh',
         ': # omitted maintenance backend', 'actual maintenance backend host entry missing'),
        ("scripts/run-sanitizer-tests.sh", 'CONFIG_REPAIR_SANITIZE=1 bash test/run-config-repair-backend.sh',
         ': # omitted maintenance backend sanitizer', 'actual maintenance backend sanitizer entry missing'),
        ("scripts/run-mock-tests.sh", 'run_gate "contextual charging command outcomes" 120 python3 test/test_command_outcomes.py',
         'true # removed contextual outcomes gate', "contextual charging command outcomes"),
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
        ("scripts/test-build-contracts.sh", 'python3 "$repo_root/test/test_deploy_candidate.py"',
         '# removed deploy consumer test', "actual signed deploy consumer must run once"),
        ("scripts/test-build-contracts.sh", 'python3 "$repo_root/test/test_deploy_candidate.py"',
         'if false; then python3 "$repo_root/test/test_deploy_candidate.py"; fi', "actual signed deploy consumer must run once"),
        ("scripts/test-build-contracts.sh", 'python3 "$repo_root/test/test_deploy_candidate.py"',
         'exit 0\npython3 "$repo_root/test/test_deploy_candidate.py"', "actual signed deploy consumer must run once"),
        ("scripts/test-build-contracts.sh", 'python3 "$repo_root/test/test_deploy_candidate.py"',
         'python3 "$repo_root/test/test_deploy_candidate.py" &', "actual signed deploy consumer must run once"),
        ("scripts/test-build-contracts.sh", 'python3 "$repo_root/test/test_deploy_candidate.py"',
         'python3 "$repo_root/test/test_deploy_candidate.py" || true', "actual signed deploy consumer must run once"),
        ("scripts/run-mock-tests.sh", '        test/test_deploy_candidate.py \\\n',
         '', "actual signed deploy consumer tests are missing"),
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
        ("test/run-cjson-oom-tests.sh", '"$repo_root/main/ota_coordinator.cpp"',
         '"$repo_root/main/absent_coordinator.cpp"', "actual OTA native family missing main/ota_coordinator.cpp"),
        ("test/run-cjson-oom-tests.sh", '"$work/test_ota_tls_admission" normal',
         ': normal', "actual OTA native family missing"),
        ("test/run-cjson-oom-tests.sh", '-DF145_HOST_TEST_HOOKS=1',
         '-UF145_HOST_TEST_HOOKS', "actual OTA native family missing"),
        ("test/run-cjson-oom-tests.sh", '"$work/test_ota_tls_admission" budget',
         ': budget', "actual OTA native family missing"),
        ("test/run-cjson-oom-tests.sh", '"$work/test_ota_tls_admission" cleanup-cas',
         ': cleanup-cas', "actual OTA native family missing"),
        ("test/run-cjson-oom-tests.sh", '"$work/test_ota_tls_admission" cleanup-failed',
         ': cleanup-failed', "actual OTA native family missing"),
        ("test/test_runtime_boundary_contract.py", 'ota_operation_contract.py',
         'absent_ota_contract.py', "actual OTA closed source contract is not called"),
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
        ("scripts/run-sanitizer-tests.sh", "syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests heap_supervisor_tests stack_sample_tests; do",
         "syslog_runtime_tests heap_supervisor_tests; do", "config/SDK shell C and C++ sanitizer"),
        ("test/CMakeLists.txt", "add_test(NAME syslog_runtime_tests COMMAND syslog_runtime_tests)",
         "# removed syslog CTest", "syslog runtime target/source"),
        ("test/CMakeLists.txt", "COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generate_syslog_adapters.py",
         "COMMAND missing_syslog_generator", "syslog runtime target/source"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute syslog shell tests" 120 "$BUILD_DIR/syslog_runtime_tests"',
         "# removed syslog fallback", "cmake-less fallback omits the actual syslog"),
        ("scripts/run-sanitizer-tests.sh", "config_commit_tests syslog_runtime_tests crash_runtime_xtensa_tests crash_runtime_riscv_tests heap_supervisor_tests stack_sample_tests; do",
         "config_commit_tests heap_supervisor_tests; do", "config/SDK shell C and C++ sanitizer"),
        ("scripts/run-fast-tests.sh", "test/test_coredump_hash.cpp", "missing_hash.cpp",
         "fast boundary checksum/syslog"),
        ("scripts/run-fast-tests.sh", "-Itest/coredump_stubs", "-Imissing_stubs",
         "fast boundary checksum/syslog"),
        ("scripts/run-fast-tests.sh", "-pthread", "", "fast NVS concurrency"),
        ("test/generate_effect_reply_adapters.py", 'function("main.cpp", "ota_health_gate_task")',
         'function("main.cpp", "missing_task")', "actual timed/config health consumer extraction"),
        ("test/generate_effect_reply_adapters.py", 'reference_name = "ordinary_recovery_reference_ready_owned"',
         'reference_name = "unreviewed_query"', "actual owned recovery-reference extraction"),
        ("test/test_config_commit.cpp", '    reference_policy_tests();',
         '    // omitted reference policy tests', "actual owned recovery-reference extraction"),
        ("test/test_config_commit.cpp", '    timed_health_tests();',
         '    // omitted actual timed health tests', "actual timed/config health consumer extraction"),
        ("test/test_config_commit.cpp", '#include "ota_health_task.generated.hpp"',
         '// omitted actual timed health header', "actual timed/config health consumer extraction"),
        ("test/CMakeLists.txt", '../esp-idf-toolchain.txt ../main/ota_update.hpp ../main/ota_update.cpp ../main/main.cpp',
         '../esp-idf-toolchain.txt ../main/ota_update.hpp ../main/ota_update.cpp',
         "actual health adapter regeneration dependencies"),
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
        ("scripts/run-mock-tests.sh", '    run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py',
         '    # run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py',
         "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", '    run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py',
         '    if false; then\n    run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py\n    fi',
         "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", '    run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py',
         '    exit 0\n    run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py',
         "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", '    run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py',
         '    run_gate "browser pipe transport" 120 python3 test/test_browser_cdp.py &',
         "strict browser invocation flow"),
        ("test/web_ui_browser_gate.py", 'if sys.platform != "linux":',
         'if False:', "Linux-only browser launch guards"),
        ("scripts/run-mock-tests.sh", 'run_gate "locked cJSON intake and OOM" 120 bash test/run-cjson-oom-tests.sh',
         '# removed real cJSON intake tests', "locked cJSON intake/OOM"),
        ("scripts/run-mock-tests.sh", "need_tool cmake", "# removed", "cmake"),
        ("scripts/run-mock-tests.sh", "tools/agent-hooks/run_with_timeout.py", "missing.py", "bounded"),
        ("tools/agent-hooks/agent_hook.py", '[str(script), "--require-all"]', "[str(script)]", "strict"),
        ("test/CMakeLists.txt", "add_test(NAME stack_sample_tests COMMAND stack_sample_tests)",
         "# omitted stack sample CTest", "Stack sample CMake/source group"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute stack sample tests" 120 "$BUILD_DIR/stack_sample_tests"',
         '# omitted actual stack sample runtime', "actual Stack sample fast/fallback sources are incomplete"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute stack sample tests" 120 "$BUILD_DIR/stack_sample_tests"',
         'run_gate "execute stack sample tests" 120 "$BUILD_DIR/stack_sample_tests" || true', "invocation flow"),
        ("scripts/run-fast-tests.sh", '  "$BUILD_DIR/stack_sample_tests"\n',
         '  # omitted actual stack sample runtime\n', "invocation flow"),
        ("scripts/run-fast-tests.sh", '  "$BUILD_DIR/stack_sample_tests"\n',
         '  "$BUILD_DIR/stack_sample_tests" || true\n', "invocation flow"),
        ("scripts/run-sanitizer-tests.sh", '    binary="$work/build/$target"',
         '    if [ "$target" = stack_sample_tests ]; then continue; fi\n    binary="$work/build/$target"', "invocation flow"),
        ("test/CMakeLists.txt", "add_executable(runtime_boundary_tests", "add_executable(runtime_missing",
         "runtime_boundary_tests"),
        ("scripts/run-mock-tests.sh", '        test/test_runtime_boundaries.cpp test/test_background_poll.cpp test/test_ota_admission.cpp test/test_coredump_hash.cpp main/diag_log.cpp main/stack_watch.cpp main/safe_mode.cpp',
         '        test/test_runtime_boundaries.cpp test/test_background_poll.cpp test/test_ota_admission.cpp test/test_coredump_hash.cpp main/diag_log.cpp main/missing_stack_watch.cpp main/safe_mode.cpp',
         "fallback omits main/stack_watch.cpp"),
        ("scripts/run-mock-tests.sh", '        main/stack_watch.cpp \\\n',
         '        main/missing_stack_watch.cpp \\\n',
         "strict inventory omits main/stack_watch.cpp"),
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
        ("scripts/run-mock-tests.sh", 'run_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py',
         '# removed exact dependency source admission', "does not execute exact dependency source admission"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py',
         '# removed dependency source build ordering', "does not execute dependency source build ordering"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py',
         '# removed dependency harness source ordering', "does not execute dependency harness source ordering"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py',
         '# removed dependency query process lifetime', "does not execute dependency query process lifetime"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py',
         '# run_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py',
         'if false; then\nrun_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py\nfi', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py',
         'exit 0\nrun_gate "dependency query process lifetime" 120 python3 test/test_dependency_process.py', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py',
         '# run_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py',
         'if false; then\nrun_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py\nfi', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py',
         'exit 0\nrun_gate "exact dependency source admission" 120 python3 test/test_dependency_sources.py', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py',
         '# run_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py',
         'if false; then\nrun_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py\nfi', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py',
         'exit 0\nrun_gate "dependency source build ordering" 120 python3 test/test_dependency_cmake.py', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py',
         '# run_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py',
         'if false; then\nrun_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py\nfi', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py',
         'exit 0\nrun_gate "dependency harness source ordering" 120 python3 test/test_dependency_harness.py', "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", "python3 scripts/check-otadata-contract.py --self-test",
         "python3 scripts/missing-otadata-contract.py --self-test", "otadata contract gate"),
        ("scripts/run-mock-tests.sh", "python3 scripts/prepare-reused-release.py --self-test",
         "python3 scripts/missing-reused-release.py --self-test", "immutable Release reuse gate"),
        ("scripts/run-mock-tests.sh", "        scripts/check-dependency-contract.py \\\n",
         "", "strict inventory omits scripts/check-dependency-contract.py"),
        ("scripts/run-mock-tests.sh", "        scripts/check-otadata-contract.py \\\n",
         "", "strict inventory omits scripts/check-otadata-contract.py"),
        ("scripts/run-mock-tests.sh", "        scripts/prepare-reused-release.py \\\n",
         "", "strict inventory omits scripts/prepare-reused-release.py"),
        ("scripts/run-mock-tests.sh",
         "python3 scripts/check-logic-test-ownership.py --compile --self-test",
         "python3 scripts/check-logic-test-ownership.py --self-test",
         "exact logic ownership/compile"),
        ("scripts/run-mock-tests.sh", "python3 scripts/check-host-gate-contract.py",
         "python3 scripts/missing-host-gate-contract.py", "host wiring contract"),
        ("test/CMakeLists.txt", "add_test(NAME nvs_storage_tests COMMAND nvs_storage_tests)",
         "# removed nvs CTest", "nvs_storage_tests is not part"),
        ("test/CMakeLists.txt", "add_test(NAME heap_supervisor_tests COMMAND heap_supervisor_tests)",
         "# removed heap CTest", "Heap CMake generator, real sources or CTest"),
        ("test/CMakeLists.txt", "add_test(NAME heap_supervisor_tests COMMAND heap_supervisor_tests)",
         "if(FALSE)\nadd_test(NAME heap_supervisor_tests COMMAND heap_supervisor_tests)\nendif()", "Heap CMake group"),
        ("test/CMakeLists.txt", "../main/main.cpp ../main/vehicle_telemetry.cpp",
         "../main/main.cpp", "Heap CMake actual-body generator dependencies"),
        ("test/CMakeLists.txt", "${CMAKE_CURRENT_SOURCE_DIR}/generate_heap_supervisor_adapter.py",
         "${CMAKE_CURRENT_SOURCE_DIR}/missing_heap_generator.py", "Heap CMake generator, real sources or CTest"),
        ("scripts/run-mock-tests.sh", "        test/test_heap_supervisor.cpp \\\n",
         "", "strict inventory omits test/test_heap_supervisor.cpp"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute heap supervisor tests" 120 "$BUILD_DIR/heap_supervisor_tests"',
         "# omitted Heap runtime", "Heap fallback invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute heap supervisor tests" 120 "$BUILD_DIR/heap_supervisor_tests"',
         'if false; then\nrun_gate "execute heap supervisor tests" 120 "$BUILD_DIR/heap_supervisor_tests"\nfi', "Heap fallback invocation flow"),
        ("scripts/run-mock-tests.sh", 'run_gate "execute heap supervisor tests" 120 "$BUILD_DIR/heap_supervisor_tests"',
         'run_gate "execute heap supervisor tests" 120 "$BUILD_DIR/heap_supervisor_tests" || true', "Heap fallback invocation flow"),
        ("scripts/run-fast-tests.sh", '\n  "$BUILD_DIR/heap_supervisor_tests"\n',
         '\n  # omitted Heap runtime\n', "Heap fast invocation flow"),
        ("scripts/run-fast-tests.sh", '\n  "$BUILD_DIR/heap_supervisor_tests"\n',
         '\n  if false; then "$BUILD_DIR/heap_supervisor_tests"; fi\n', "Heap fast invocation flow"),
        ("scripts/run-fast-tests.sh", '\n  "$BUILD_DIR/heap_supervisor_tests"\n',
         '\n  "$BUILD_DIR/heap_supervisor_tests" || true\n', "Heap fast invocation flow"),
        ("scripts/run-sanitizer-tests.sh", '    binary="$work/build/$target"',
         '    if [ "$target" = heap_supervisor_tests ]; then continue; fi\n    binary="$work/build/$target"', "sanitizer invocation flow"),
        ("scripts/run-sanitizer-tests.sh", 'LSAN_OPTIONS="$lsan_options" "$binary"\ndone',
         'LSAN_OPTIONS="$lsan_options" "$binary" || true\ndone', "sanitizer invocation flow"),
        ("test/CMakeLists.txt", "test_nvs_storage.cpp", "missing_nvs_storage.cpp",
         "target/source wiring"),
        ("scripts/run-mock-tests.sh",
         'run_gate "execute NVS adapter tests" 120 "$BUILD_DIR/nvs_storage_tests"',
         "# removed NVS fallback execution", "compile and execute nvs_storage_tests"),
        ("test/web_ui_browser_gate.py", "STARTUP_SECONDS = 45",
         "STARTUP_SECONDS = 20", "absolute work/startup and termination budgets"),
        ("test/web_ui_browser_gate.py", '("tablet", 768, 900, "light")',
         '("tablet", 769, 900, "light")', "three populated browser profiles"),
        ("test/web_ui_browser_gate.py", 'value["actions"] == index and len(value["clicks"]) == index',
         "True", "trusted native keyboard/focus acceptance"),
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
    css_call = 'run_gate "CSS bytes and actual inline generation" 120 python3 test/test_inline_assets.py'
    mutations.extend([
        ("scripts/run-mock-tests.sh", css_call, "true # omitted CSS generator controls",
         "execute actual CSS inline generation"),
        ("scripts/run-mock-tests.sh", css_call, 'if false; then\n' + css_call + '\nfi',
         "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", css_call, 'exit 0\n' + css_call,
         "strict browser invocation flow"),
    ])
    stack_call = 'run_gate "stack context owner hooks" 120 python3 test/test_stack_context_hooks.py . --self-test'
    for replacement in (
        'true # omitted stack context owner gate',
        '# ' + stack_call,
        'if false; then ' + stack_call + '; fi',
        'exit 0\n' + stack_call,
        stack_call + ' || true',
        stack_call.replace('120 python3', '119 python3'),
        stack_call.replace('. --self-test', '.'),
    ):
        mutations.append(("scripts/run-mock-tests.sh", stack_call, replacement,
                          "stack hook invocation flow"))
    for path in ("test/test_heap_supervisor.cpp", "test/test_clock_runtime.cpp",
                 "test/test_config_commit.cpp", "test/test_effect_reply_adapters.cpp"):
        mutations.append((path, '#include "stack_watch.hpp"', '// removed actual stack header',
                          "F135 actual declaration/no-noop contract"))
        mutations.append((path, 'CHECK(task == nullptr); ++f135_hwm_queries;',
                          '(void)task; ++f135_hwm_queries;', "F135 own-task HWM spy/actual hook oracle"))
    mutations.extend([
        ("test/CMakeLists.txt", "../main/heap_trend.cpp ../main/stack_watch.cpp)",
         "../main/heap_trend.cpp)", "F135 CMake real sampler join: heap_supervisor_tests"),
        ("test/CMakeLists.txt", "clock_runtime_tests test_clock_runtime.cpp ../main/stack_watch.cpp",
         "clock_runtime_tests test_clock_runtime.cpp", "F135 CMake real sampler join: clock_runtime_tests"),
        ("test/CMakeLists.txt", "config_commit_tests test_config_commit.cpp ../main/stack_watch.cpp",
         "config_commit_tests test_config_commit.cpp", "F135 CMake real sampler join: config_commit_tests"),
        ("test/run-cjson-oom-tests.sh", '"$repo_root/test/test_effect_reply_adapters.cpp" "$repo_root/main/stack_watch.cpp"',
         '"$repo_root/test/test_effect_reply_adapters.cpp"', "F135 cJSON real sampler/instrumented SDK stub join"),
    ])
    for path, old, new in (
        ("scripts/run-mock-tests.sh", "test/test_heap_supervisor.cpp main/safe_mode.cpp main/heap_trend.cpp main/stack_watch.cpp",
         "test/test_heap_supervisor.cpp main/safe_mode.cpp main/heap_trend.cpp"),
        ("scripts/run-mock-tests.sh", "test/test_clock_runtime.cpp main/stack_watch.cpp", "test/test_clock_runtime.cpp"),
        ("scripts/run-mock-tests.sh", 'test/test_config_commit.cpp "$BUILD_DIR/idf_ota_state_query.o" main/stack_watch.cpp',
         'test/test_config_commit.cpp "$BUILD_DIR/idf_ota_state_query.o"'),
        ("scripts/run-fast-tests.sh", "test/test_heap_supervisor.cpp main/safe_mode.cpp main/heap_trend.cpp main/stack_watch.cpp",
         "test/test_heap_supervisor.cpp main/safe_mode.cpp main/heap_trend.cpp"),
    ):
        mutations.append((path, old, new, "F135 " + ("fast" if "fast" in path else "fallback") + " real sampler join"))
    cmake_source = (root / "test/CMakeLists.txt").read_text()
    heap_begin = cmake_source.index("    add_custom_command(\n        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/heap_supervisor_adapter.generated.hpp")
    heap_end = cmake_source.index("\n\n    add_custom_command(", heap_begin + 1)
    heap_fixture = cmake_source[heap_begin:heap_end]
    for equals in ("", "="):
        mutations.append(("test/CMakeLists.txt", heap_fixture,
                          "#[" + equals + "[\n" + heap_fixture + "\n]" + equals + "]",
                          "Heap CMake group must have only its Python-availability condition"))
    f137_call = 'python3 test/test_f137_host.py --source . --controls --execute-controlled-host-only'
    f137_required_line = '        test/test_f137_host.py ' + chr(92) + '\n'
    f137_cmake = (root / "test/CMakeLists.txt").read_text()
    f137_start = f137_cmake.index('# Whole actual task bodies')
    f137_end = f137_cmake.index('# Actual crash shell', f137_start)
    f137_group = f137_cmake[f137_start:f137_end]
    mutations.extend([
        ("scripts/run-mock-tests.sh", f137_required_line, '', "F137 host wiring"),
        ("scripts/run-mock-tests.sh", '    run_gate "MQTT/WDT and network iteration bodies" 120 ' + f137_call,
         '    if false; then run_gate "MQTT/WDT and network iteration bodies" 120 ' + f137_call + '; fi', "strict browser invocation flow"),
        ("scripts/run-fast-tests.sh", '  ' + f137_call + '\n', '  if false; then\n  ' + f137_call + '\n  fi\n', "F137 host wiring"),
        ("scripts/run-fast-tests.sh", '  ' + f137_call + '\n', '  ' + f137_call + ' || true\n#  ' + f137_call + '\n', "F137 host wiring"),
        ("scripts/run-sanitizer-tests.sh", 'python3 test/test_f137_host.py --source . --sanitize --execute-controlled-host-only',
         'true # python3 test/test_f137_host.py --source . --sanitize --execute-controlled-host-only', "sanitizer invocation flow"),
        ("test/CMakeLists.txt", f137_group, '#[[\n' + f137_group + ']]\n', "F137 host wiring"),
    ])
    relevance_gate = '    run_gate "release relevance and publication selectors" 120 scripts/release-relevance.sh --self-test'
    build_gate = '    run_gate "build and manifest contract tests" 120 scripts/test-build-contracts.sh --build-and-manifest'
    for command in (relevance_gate, build_gate):
        for changed in ('# ' + command, '    if false; then\n' + command + '\n    fi',
                        '    exit 0\n' + command, '    (' + command.strip() + ') || true', command + ' &'):
            mutations.append(("scripts/run-mock-tests.sh", command, changed, "strict browser invocation flow"))
    mutations.extend([
        ("scripts/run-mock-tests.sh", relevance_gate + '\n' + build_gate,
         build_gate + '\n' + relevance_gate, "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", build_gate, build_gate.replace(' --build-and-manifest', ''),
         "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", build_gate, build_gate.replace('--build-and-manifest', '--unknown'),
         "strict browser invocation flow"),
        ("scripts/run-mock-tests.sh", '        scripts/release-relevance.sh ' + chr(92) + '\n', '',
         "run-mock-tests.sh strict inventory omits scripts/release-relevance.sh"),
        ("scripts/test-build-contracts.sh", 'build_and_manifest_only=0', 'build_and_manifest_only=1',
         "build-contract profile invocation flow"),
        ("scripts/test-build-contracts.sh", '  "$repo_root/scripts/release-relevance.sh" --self-test',
         '  : # omitted default relevance', "build-contract profile invocation flow"),
        ("scripts/test-build-contracts.sh", '  "$repo_root/scripts/release-relevance.sh" --self-test',
         '  "$repo_root/scripts/release-relevance.sh" --self-test || true', "build-contract profile invocation flow"),
        ("scripts/test-build-contracts.sh", 'if [[ "$build_and_manifest_only" -eq 0 ]]; then', 'if true; then',
         "build-contract profile invocation flow"),
        ("scripts/test-build-contracts.sh", '[[ "$1" == --build-and-manifest ]] || {', 'true || {',
         "build-contract profile invocation flow"),
        ("scripts/test-build-contracts.sh", '  *) echo "usage: test-build-contracts.sh [--build-and-manifest]" >&2; exit 2 ;;',
         '  *) build_and_manifest_only=1 ;;', "build-contract profile invocation flow"),
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
                "test/test_f137_host.py",
                'test/f137_process.py',
                'test/f137_process_controls.py',
                'test/f137_driver_controls.py',
                'test/f137_task_lsan.hpp',
                "test/generate_f137_host_adapters.py",
                "test/f137_mqtt_host_template.cpp",
                "test/f137_net_host_template.cpp",
                "test/f137_stubs/esp_task_wdt.h",
                "test/f137_stubs/ping/ping_sock.h",
                "test/f137-callback-probe-v1.py",
                "test/f137-callback-probe-template-v1.cpp",
                "test/test_f137_host_wiring.py",
                "test/generate_effect_reply_adapters.py",
                "test/test_config_commit.cpp",
                "test/test_heap_supervisor.cpp",
                "test/test_clock_runtime.cpp",
                "test/test_effect_reply_adapters.cpp",
                "test/test_stack_sample.cpp",
                "test/run-cjson-oom-tests.sh",
                "test/web_ui_browser_gate.py",
                "test/browser_cdp.py",
                "test/run-mqtt-json-publish-tests.sh",
                "test/test_mqtt_json_publish.cpp",
                "test/run-cjson-oom-tests.sh",
                "test/test_runtime_boundary_contract.py",
            ):
                destination = fixture / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(root / path, destination)
            replace_once(fixture / relative, old, new)
            if relative == "scripts/run-sanitizer-tests.sh" and old == IDENTITY_SANITIZER_COMMAND_BLOCK:
                syntax = subprocess.run(["/bin/bash", "-n", str(fixture / relative)],
                                        capture_output=True, timeout=5)
                require(syntax.returncode == 0,
                        "disabled identity sanitizer canary must preserve valid Bash syntax")
            try:
                validate(fixture)
            except ContractError as exc:
                require(expected in str(exc),
                        f"self-test mutation failed for wrong reason in {relative} replacing {old!r}: {exc}")
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
