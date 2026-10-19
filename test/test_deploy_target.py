#!/usr/bin/env python3
"""Execute the actual bench acceptance recipes with inert HTTP and E2E command doubles."""

from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
TARGET = "192.0.2.145"


def recipe(source: str, section: str) -> str:
    match = re.search(r"### " + section + r"\..*?```bash\n(.*?)```", source, re.S)
    assert match, f"missing {section} recipe"
    return match[1]


def run(block: str, have_target: bool, fail_diag: bool = False) -> tuple[int, list[str]]:
    with tempfile.TemporaryDirectory() as directory:
        calls = Path(directory) / "calls"
        # No nounset here: each published recipe must refuse an absent target itself.
        prefix = "set -eo pipefail\n"
        prefix += "unset DEVICE_IP\n"
        prefix += f"FAIL_DIAG={int(fail_diag)}\n"
        prefix += "TARGET_HOST=198.51.100.9\n"
        prefix += "export RUN_COMMANDS=1 ALLOW_CHARGE_TOGGLE=1 RUN_ALL_COMMANDS=1\n"
        if have_target:
            prefix += f"DEVICE_IP={TARGET}\n"
        prefix += f"calls={shlex.quote(str(calls))}\n"
        prefix += r'''
curl() {
  for argument in "$@"; do
    case "$argument" in http://*) printf '%s\n' "$argument" >> "$calls" ;; esac
  done
  case "$*" in *"/diag?redact=1"*) [ "$FAIL_DIAG" = 0 ] || return 22 ;; esac
  printf '{}\n'
}
jq() { while IFS= read -r line; do :; done; printf '{}\n'; }
grep() { return 1; }
bash() {
  printf '%s %s %s %s %s\n' "${ESP32_URL-unset}" "${RUN_COMMANDS-unset}" \
    "${ALLOW_CHARGE_TOGGLE-unset}" "${RUN_ALL_COMMANDS-unset}" "$*" >> "$calls"
}
PATH=/nonexistent
'''
        completed = subprocess.run(
            ["bash", "-c", prefix + block], check=False, capture_output=True,
            text=True, timeout=10,
        )
        return completed.returncode, calls.read_text().splitlines() if calls.exists() else []


def check(health: str, e2e: str) -> None:
    for block, expected in (
        (health, [f"http://{TARGET}/status", f"http://{TARGET}/api/proxy/1/version",
                  f"http://{TARGET}/diag?redact=1"]),
        (e2e, [f"http://{TARGET} 0 0 0 scripts/e2e_evcc.sh"]),
    ):
        status, calls = run(block, True)
        assert status == 0 and calls == expected, (status, calls, expected)
        status, calls = run(block, False)
        assert status != 0 and not calls, ("missing target must stop before any request", status, calls)

    status, calls = run(health, True, fail_diag=True)
    assert status != 0 and calls[-1:] == [f"http://{TARGET}/diag?redact=1"], (
        "diagnostic transport failure must not be accepted as a healthy bench", status, calls)


def main() -> None:
    source = (ROOT / ".agents/skills/deploy-bench/SKILL.md").read_text()
    health, e2e = recipe(source, "5b"), recipe(source, "5c")
    check(health, e2e)
    controls = (
        (health.replace("$DEVICE_IP", "$TARGET_HOST"), e2e),
        (health.replace(") || exit 1", ") || true"), e2e),
        (health, e2e.replace('ESP32_URL="http://$DEVICE_IP" ', "")),
        (re.sub(r'^: .*\n', '', health, count=1, flags=re.M), e2e),
        (health, re.sub(r'^: .*\n', '', e2e, count=1, flags=re.M)),
        (health, e2e.replace("RUN_COMMANDS=0 ", "")),
        (health, e2e.replace("ALLOW_CHARGE_TOGGLE=0 ", "")),
        (health, e2e.replace("RUN_ALL_COMMANDS=0 ", "")),
    )
    for health_control, e2e_control in controls:
        assert (health_control, e2e_control) != (health, e2e), "control did not apply"
        try:
            check(health_control, e2e_control)
        except AssertionError:
            continue
        raise AssertionError("unsafe target-binding mutation accepted")
    print(f"deploy-target: PASS (selected/missing target, read-only modes, {len(controls)} negative controls; no live calls)")


if __name__ == "__main__":
    main()
