#!/usr/bin/env python3
"""Offline positive/negative controls for narrow hook read exceptions; never execute payloads."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
HOOKS = ROOT / "tools/agent-hooks"
sys.path.insert(0, str(HOOKS))
from shell_read import is_literal_read
from merge_payload import find_merge

spec = importlib.util.spec_from_file_location("read_guard_under_test", HOOKS / "agent_hook.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def allowed(command: str, *, partition: bool = False, cwd: Path = ROOT,
            extra: dict | None = None) -> bool:
    argv = ([sys.executable, str(HOOKS / "agent_hook.py"), "pre-tool-guards"] if partition
            else ["bash", str(HOOKS / "require-pr-gates.sh")])
    payload = {"tool_name": "Bash", "cwd": str(cwd), "tool_input": {"command": command}}
    payload["tool_input"].update(extra or {})
    result = subprocess.run(argv, input=json.dumps(payload), text=True, capture_output=True,
                            cwd=ROOT, timeout=20)
    # The secret/partition hook emits a JSON deny even when its process exits zero.
    return result.returncode == 0 and not (result.stdout + result.stderr).strip()


reads = [
    "rg --no-config -n 'git push|gh pr merge' AGENTS.md",
    "rg --no-config -e 'mergePullRequest' -g '*.md' docs",
    "rg --no-config '$(gh pr merge 123)' AGENTS.md",
    'rg --no-config "gh pr merge" "file with spaces.md"',
    "rg --no-config 'gh pr merge # quoted data' AGENTS.md",
    "gh pr list --json number,title --jq '.[] | select(.title | test(\"gh pr merge\"))'",
    "gh --repo github.com/0Bu/tesla-key-esp32 pr view 123 --json body --jq '.body | test(\"mergePullRequest\")'",
    "gh run list --json name --jq '.[] | select(.name == \"gh pr merge\")'",
]
for command in reads:
    assert is_literal_read(command), command
    assert find_merge(command) is None, command
    assert allowed(command), command

not_literal = [
    "rg -n 'git push|gh pr merge' AGENTS.md",  # inherited rg config is not bounded
    "rg -e --no-config 'git push|gh pr merge' AGENTS.md",
    "rg 'git push|gh pr merge' -- --no-config",
    "rg 'gh pr merge' AGENTS.md # --no-config",
    "gh pr view 123 # --json body --jq 'gh pr merge'",
    "rg --no-config --pre 'gh pr merge 123' AGENTS.md",
    "rg --no-config --hostname-bin 'gh pr merge 123' AGENTS.md",
    "rg --no-config 'gh pr merge' *",
    "rg --no-config 'gh pr merge' ?",
    "rg --no-config 'gh pr merge' [a-z]*",
    "rg --no-config 'gh pr merge' ~/docs",
    "rg --no-config \"$(gh pr merge 123)\" AGENTS.md",
    'rg --no-config "`gh pr merge 123`" AGENTS.md',
    "rg --no-config x AGENTS.md; gh pr merge 123",
    "rg --no-config x AGENTS.md | sh -c 'gh pr merge 123'",
    "sh -c \"rg --no-config 'gh pr merge' AGENTS.md\"",
    "eval 'gh pr merge 123'",
    "$tool pr merge 123",
    "env -S 'gh pr merge 123'",
    "gh pr list --search --json --jq 'gh pr merge'",
    "gh pr list --json title --search --jq 'gh pr merge'",
    "gh pr list -- --json title --jq 'gh pr merge'",
    "gh pr view 123 --web --json body --jq 'gh pr merge'",
    "gh pr merge 123 --json body --jq 'gh pr merge'",
]
for command in not_literal:
    assert not is_literal_read(command), command
    assert not allowed(command), command

# Publication classification must not turn off the separately invoked secret/partition guard.
assert not allowed("rg --no-config 'gh pr merge' ota_signing_key.pem", partition=True)
assert not allowed("rg --no-config 'gh pr merge' nvs-backup.bin", partition=True)

reader = "python3 -I -S scripts/check-partition-contract.py --csv partitions.csv"
assert allowed(reader, partition=True)
negative_reads = [
    "python3 scripts/check-partition-contract.py --csv partitions.csv",
    reader.replace(" -I", ""), reader.replace(" -S", ""),
    reader.replace("python3", "/usr/bin/python3"),
    reader.replace("scripts/check-partition-contract.py", "scripts/other.py"),
    reader.replace("scripts/check-partition-contract.py", str(HOOKS / "check-partition-contract.py")),
    reader + " --self-test", reader + " --output partitions.csv",
    reader + " > partitions.csv", reader + "; printf test > partitions.csv",
    reader + " --binary missing-table.bin",
    "python3 -c 'open(\"partitions.csv\",\"w\").write(\"x\")'",
]
for command in negative_reads:
    assert not allowed(command, partition=True), command
assert not guard.canonical_partition_read(reader.replace("--csv partitions.csv", "--csv other.csv"), ROOT, ROOT)

(ROOT / ".tmp").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(dir=ROOT / ".tmp", prefix="hook-read-") as work:
    work = Path(work)
    binary = work / "installed-partition-table.bin"
    # Guard classification checks path ownership; it does not inspect or read table bytes.
    binary.write_bytes(b"offline table fixture")
    assert allowed(reader + " --binary " + str(binary), partition=True)
    alias = work / "table-link.bin"
    alias.symlink_to(binary)
    assert not allowed(reader + " --binary " + str(alias), partition=True)
    directory_alias = work / "directory-link"
    directory_alias.symlink_to(work, target_is_directory=True)
    assert not allowed(reader + " --binary " + str(directory_alias / binary.name), partition=True)
    assert not allowed(reader, partition=True, cwd=work)
    assert not allowed(reader, partition=True, extra={"workdir": str(work)})
    assert not allowed(reader, partition=True, extra={"workdir": str(work), "cwd": str(ROOT)})
    assert not allowed(reader, partition=True, extra={"workdir": [str(ROOT)]})
    assert allowed(reader, partition=True, extra={"workdir": "."})
    foreign = work / "foreign"
    (foreign / "scripts").mkdir(parents=True)
    script = foreign / "scripts/check-partition-contract.py"
    shutil.copyfile(ROOT / "scripts/check-partition-contract.py", script)
    shutil.copyfile(ROOT / "partitions.csv", foreign / "partitions.csv")
    assert guard.canonical_partition_read(reader, foreign, foreign)
    (foreign / "~").mkdir()
    (foreign / "~/table.bin").write_bytes(b"offline table fixture")
    for meta in ("*.bin", "?.bin", "[a-z].bin"):
        (foreign / meta).write_bytes(b"offline table fixture")
    for meta in ("~/table.bin", "*.bin", "?.bin", "[a-z].bin"):
        assert not guard.canonical_partition_read(reader + " --binary " + meta, foreign, foreign), meta
    script.write_bytes(script.read_bytes() + b"\n# changed fixture\n")
    assert not guard.canonical_partition_read(reader, foreign, foreign)
    script.unlink()
    script.symlink_to(ROOT / "scripts/check-partition-contract.py")
    assert not guard.canonical_partition_read(reader, foreign, foreign)
with tempfile.TemporaryDirectory(prefix="hook-read-outside-") as outside:
    foreign_binary = Path(outside) / "table.bin"
    foreign_binary.write_bytes(b"offline table fixture")
    assert not allowed(reader + " --binary " + str(foreign_binary), partition=True)

canonical = "gh --repo github.com/0Bu/tesla-key-esp32 pr merge 123 --match-head-commit " + "a" * 40 + " --squash"
parsed = find_merge(canonical)
assert parsed and parsed["action"] == "gh pr merge" and not parsed["error"]
print(f"hook read contract: PASS ({len(reads)} publication reads, {len(not_literal)} conservative cases, "
      f"{len(negative_reads)} invalid reader forms, identity/hash/path/secret controls)")
