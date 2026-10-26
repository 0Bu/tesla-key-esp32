#!/usr/bin/env python3
"""Actual harness optional/strict admission with passive Git/compiler effects, no network."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
from test_dependency_cmake import ROOT, copy_repository_inputs


def main():
    call = ('python3 "$ROOT_DIR/scripts/dependency_source.py" harness-preflight \\\n'
            '    --root "$ROOT_DIR" --source "$CACHE_DIR" >/dev/null\n')
    assert (ROOT / "scripts/test-tesla-ble-harness.sh").read_text().count(call) == 1
    cases = []
    with tempfile.TemporaryDirectory(prefix="dependency-harness-admission-") as temporary:
        work = Path(temporary).resolve()
        omitted = work / "omitted"
        omitted.mkdir()
        copy_repository_inputs(omitted)
        harness = omitted / "scripts/test-tesla-ble-harness.sh"
        harness.write_text(harness.read_text().replace(call, "if false; then\n" + call + "fi\n", 1))
        def case(name, *, foreign=None, strict=False, dns=True, root=ROOT,
                 expected=1, fetch_expected=False):
            directory = work / name
            directory.mkdir()
            cache = directory / "cache"
            cache.mkdir()
            fetch, compiler = directory / "fetch-effect", directory / "compiler-effect"
            witness = None
            if foreign == "orphan":
                witness = cache / ".tesla-pristine.tesla-source.json"
                witness.write_bytes(b"foreign owner remains")
            elif foreign:
                target = cache / foreign
                target.mkdir()
                witness = target / "sentinel"
                witness.write_bytes(b"foreign remains")
            original = witness.read_bytes() if witness else None
            tools = directory / "tools"
            tools.mkdir()
            diagnostic = ("fatal: Could not resolve host: github.com\n" if dns else
                          "fatal: remote error: upload-pack: not our ref " + "0" * 40 + "\n")
            git = tools / "git"
            git.write_text("#!" + sys.executable + "\nfrom pathlib import Path\nimport sys\n"
                "assert 'fetch' in sys.argv\nPath(" + repr(str(fetch)) + ").write_text('cold fetch')\n"
                "sys.stderr.write(" + repr(diagnostic) + ")\nraise SystemExit(128)\n")
            git.chmod(0o700)
            for tool_name in ("clang", "clang++"):
                tool = tools / tool_name
                tool.write_text("#!" + sys.executable + "\nfrom pathlib import Path\nPath(" +
                                repr(str(compiler)) + ").write_text('unsafe compiler effect')\n"
                                "raise SystemExit(77)\n")
                tool.chmod(0o700)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("GIT_", "SSH_", "PYTHON", "BASH_FUNC_"))
                   and key not in ("GH_TOKEN", "GITHUB_TOKEN", "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE",
                                   "ENV", "BASH_ENV", "CI")}
            env.update(PATH=str(tools) + os.pathsep + os.environ["PATH"],
                       BUILD_DIR=str(directory / "build"), CACHE_DIR=str(cache), CC="clang", CXX="clang++")
            command = ["bash", "scripts/test-tesla-ble-harness.sh"] + (["--require-all"] if strict else [])
            result = subprocess.run(command, cwd=root, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=30)
            assert result.returncode == expected, (name, result.returncode, result.stderr.decode())
            assert fetch.exists() == fetch_expected, name
            assert not compiler.exists(), name
            assert witness is None or witness.read_bytes() == original, name
            cases.append(name)
        case("foreign-tesla-before-offline", foreign="tesla-pristine")
        case("foreign-nanopb-before-offline", foreign="nanopb-pristine")
        case("foreign-workspace-before-offline", foreign="tesla-patched")
        case("orphan-owner-before-offline", foreign="orphan")
        case("legitimate-optional-cold-dns", expected=0, fetch_expected=True)
        case("strict-cold-dns", strict=True, fetch_expected=True)
        case("unknown-commit-hard-failure", dns=False, fetch_expected=True)
        case("effective-preflight-omission", foreign="tesla-pristine", root=omitted,
             expected=0, fetch_expected=True)
        case("restored-preflight-refuses", foreign="tesla-pristine")
    print(json.dumps({"result": "PASS", "cases": len(cases),
                      "limits": "Actual harness plus real source reader with synthetic transport and passive compilers; no network, protocol compilation, signing or device proof."}))


if __name__ == "__main__":
    main()
