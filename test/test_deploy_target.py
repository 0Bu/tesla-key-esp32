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
        prefix += "export BENCH_UNCONFIGURED=1\n"
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


def check(health: str, e2e: str, snapshot: str) -> None:
    diagnostics = [f"http://{TARGET}/api/proxy/1/version", f"http://{TARGET}/diag?redact=1"]
    for block, expected in (
        (health, diagnostics),
        (e2e, [f"http://{TARGET} 0 0 0 scripts/e2e_evcc.sh"]),
        (snapshot, [f"http://{TARGET}/status"]),
    ):
        status, calls = run(block, True)
        assert status == 0 and calls == expected, (status, calls, expected)
        status, calls = run(block, False)
        assert status != 0 and not calls, ("missing target must stop before any request", status, calls)

    status, calls = run(health, True, fail_diag=True)
    assert status != 0 and calls[-1:] == [f"http://{TARGET}/diag?redact=1"], (
        "diagnostic transport failure must not be accepted as a healthy bench", status, calls)


def check_recovery_scope(scrub: str, recipes: list[str]) -> None:
    prefix = """
export BOOTLOADER=boot.bin PARTITION_TABLE=table.bin BOOT_OFFSET=0x0
TARGET=esp32s3 PORT=/dev/cu.EXPLICIT_TEST APP=app.bin
esptool() { printf '%s\\n' "$*"; }
"""
    for recipe_block, reset_args in zip(recipes, ("", "--before no-reset --after no-reset "), strict=True):
        result = subprocess.run(["bash", "-c", prefix + scrub + recipe_block],
                                capture_output=True, text=True, timeout=10)
        port_args = f"--chip esp32s3 -p /dev/cu.EXPLICIT_TEST {reset_args}"
        expected = [f"{port_args}write_flash 0x20000 app.bin", f"{port_args}erase_region 0xf000 0x2000"]
        assert result.returncode == 0 and result.stdout.splitlines() == expected, result


def check_local_port(block: str) -> None:
    """Run the published port gate before chip probing with a passive USB double."""
    for have_port in (False, True):
        with tempfile.TemporaryDirectory() as directory:
            calls, selected = Path(directory) / "calls", Path(directory) / "selected"
            prefix = "set -e\nunset PORT\n"
            prefix += f"calls={shlex.quote(str(calls))}\n"
            prefix += r'''
ioreg() {
  printf 'ioreg\n' >> "$calls"
  printf '"USB Product Name" = "USB Single Serial"\n'
  printf '"IOCalloutDevice" = "/dev/cu.usbmodemUNRELATED"\n'
}
'''
            if have_port:
                prefix += "PORT=/dev/cu.EXPLICIT_TEST\n"
            completed = subprocess.run(
                ["bash", "-c", prefix + block + '\nprintf "%s" "${PORT-}" > '
                 + shlex.quote(str(selected))], capture_output=True, text=True, timeout=10,
            )
            assert not calls.exists(), "port gate must not select a USB bridge"
            if have_port:
                assert completed.returncode == 0 and selected.read_text() == "/dev/cu.EXPLICIT_TEST"
            else:
                assert completed.returncode != 0 and not selected.exists(), "absent PORT must stop"


def main() -> None:
    source = (ROOT / ".agents/skills/deploy-bench/SKILL.md").read_text()
    health, e2e, snapshot = (recipe(source, section) for section in ("5b", "5c", "5d"))
    check(health, e2e, snapshot)
    controls = (
        (health.replace("$DEVICE_IP", "$TARGET_HOST"), e2e),
        (health.replace(") || exit 1", ") || true"), e2e),
        (health + 'if [ "${BENCH_UNCONFIGURED:-0}" = 1 ]; then\n' + snapshot + 'fi\n', e2e),
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
            check(health_control, e2e_control, snapshot)
        except AssertionError:
            continue
        raise AssertionError("unsafe target-binding mutation accepted")

    delegate_section = source.split("## 4. Execute the flash procedure", 1)[1].split("\n## 5.", 1)[0]
    [scrub] = re.findall(r"```bash\n(.*?)```", delegate_section, re.S)
    recovery = (ROOT / ".agents/skills/usb-recovery/SKILL.md").read_text()
    write_sections = recovery.split("**s3 / c3 / c6 / classic esp32**", 1)[1].split("\n## 4.", 1)[0]
    recovery_writes = re.findall(r"```bash\n(.*?)```", write_sections, re.S)
    check_recovery_scope(scrub, recovery_writes)
    try:
        check_recovery_scope("", recovery_writes)
    except AssertionError:
        pass
    else:
        raise AssertionError("delegated recovery accepted inherited support-image writes")

    flash = (ROOT / ".agents/skills/flash-esp32/SKILL.md").read_text()
    port_gate = flash.split("# Flash from the HOST", 1)[1].split("# Detect the chip", 1)[0]
    port_gate = "# Flash from the HOST" + port_gate
    check_local_port(port_gate)
    port_controls = (
        re.sub(r'^: .*\n', '', port_gate, count=1, flags=re.M),
        'if [ -z "${PORT:-}" ]; then PORT=/dev/cu.AUTO_SELECTED; fi\n' + port_gate,
    )
    for control in port_controls:
        assert control != port_gate, "port control did not apply"
        try:
            check_local_port(control)
        except AssertionError:
            continue
        raise AssertionError("unsafe local port selection accepted")
    print(f"deploy-target: PASS (target/port binding, read-only modes, app-only writes, "
          f"{len(controls) + len(port_controls) + 1} negative controls; no live calls)")


if __name__ == "__main__":
    main()
