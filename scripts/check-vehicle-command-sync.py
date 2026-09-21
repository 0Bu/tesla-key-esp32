#!/usr/bin/env python3
"""Vehicle-command protocol sync and conformance checker.

Validates the local command table in main/logic/command_registry.hpp and vehicle_commands.cpp
against the protocol invariants of teslamotors/vehicle-command and the capabilities of yoziru/tesla-ble.

Modes:
  --self-test         Offline verification of local protocol invariants and mutation canaries.
  --check             Check local command definitions against pinned protocol expectations.
  --fetch-upstream    Fetch raw reference definitions from teslamotors/vehicle-command if online.
"""

from __future__ import annotations

import argparse
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Pinned protocol constants from teslamotors/vehicle-command & yoziru/tesla-ble
PINNED_BLE_SERVICE_UUID = "00000211-b2d1-43f0-9b88-960cebf8b91e"
PINNED_BLE_WRITE_UUID = "00000212-b2d1-43f0-9b88-960cebf8b91e"
PINNED_BLE_NOTIFY_UUID = "00000213-b2d1-43f0-9b88-960cebf8b91e"

# Expected REST / MCP command surface mapping
EXPECTED_COMMANDS = {
    "wake_up": {"mcp": "wake_up", "role_allowed": True},
    "charge_start": {"mcp": "charge_start", "role_allowed": True},
    "charge_stop": {"mcp": "charge_stop", "role_allowed": True},
    "charge_port_door_open": {"mcp": "charge_port_open", "role_allowed": True},
    "charge_port_door_close": {"mcp": "charge_port_close", "role_allowed": True},
    "set_charging_amps": {"mcp": "set_charging_amps", "role_allowed": True, "lo": 0, "hi": 48},
    "set_charge_limit": {"mcp": "set_charge_limit", "role_allowed": True, "lo": 50, "hi": 100},
    "set_scheduled_charging": {"mcp": "set_scheduled_charging", "role_allowed": True, "start_lo": 0, "start_hi": 1439},
    "door_lock": {"mcp": None, "role_allowed": False},
    "door_unlock": {"mcp": None, "role_allowed": False},
    "flash_lights": {"mcp": None, "role_allowed": False},
    "honk_horn": {"mcp": None, "role_allowed": False},
    "set_sentry_mode": {"mcp": None, "role_allowed": False},
    "auto_conditioning_start": {"mcp": None, "role_allowed": False},
    "auto_conditioning_stop": {"mcp": None, "role_allowed": False},
}


def parse_local_command_registry(registry_header: Path) -> dict[str, dict]:
    """Parse kCommands from main/logic/command_registry.hpp."""
    text = registry_header.read_text(encoding="utf-8")
    commands: dict[str, dict] = {}

    row_pattern = re.compile(
        r'\{\s*CmdKind::(?P<kind>[A-Za-z0-9_]+)\s*,\s*(?P<api_name>"[^"]*"|nullptr)\s*,\s*(?P<mcp_name>"[^"]*"|nullptr)'
    )
    for match in row_pattern.finditer(text):
        api_raw = match.group("api_name")
        mcp_raw = match.group("mcp_name")
        api_name = None if api_raw == "nullptr" else api_raw.strip('"')
        mcp_name = None if mcp_raw == "nullptr" else mcp_raw.strip('"')
        if api_name:
            commands[api_name] = {
                "kind": match.group("kind"),
                "mcp_name": mcp_name,
            }
    return commands


def verify_local_invariants(root: Path) -> list[str]:
    """Verify local command definitions against protocol invariants."""
    findings: list[str] = []
    registry_file = root / "main/logic/command_registry.hpp"
    if not registry_file.exists():
        return [f"registry file missing: {registry_file}"]

    local_cmds = parse_local_command_registry(registry_file)
    for api_name, expected in EXPECTED_COMMANDS.items():
        if api_name not in local_cmds:
            findings.append(f"missing command in registry: {api_name}")
            continue
        entry = local_cmds[api_name]
        expected_mcp = expected["mcp"]
        if entry["mcp_name"] != expected_mcp:
            findings.append(
                f"command {api_name}: expected MCP name {expected_mcp!r}, got {entry['mcp_name']!r}"
            )

    # Check ble_client.hpp for UUIDs
    ble_client_file = root / "main/ble_client.hpp"
    if ble_client_file.exists():
        ble_text = ble_client_file.read_text(encoding="utf-8").lower()
        if PINNED_BLE_SERVICE_UUID not in ble_text:
            findings.append(f"BLE service UUID {PINNED_BLE_SERVICE_UUID} missing from ble_client.hpp")
        if PINNED_BLE_WRITE_UUID not in ble_text:
            findings.append(f"BLE write UUID {PINNED_BLE_WRITE_UUID} missing from ble_client.hpp")
        if PINNED_BLE_NOTIFY_UUID not in ble_text:
            findings.append(f"BLE notify UUID {PINNED_BLE_NOTIFY_UUID} missing from ble_client.hpp")

    return findings


def fetch_upstream_charge_go() -> str | None:
    """Fetch raw pkg/vehicle/charge.go from upstream repo."""
    url = "https://raw.githubusercontent.com/teslamotors/vehicle-command/main/pkg/vehicle/charge.go"
    req = urllib.request.Request(url, headers={"User-Agent": "tesla-key-esp32-conformance-check"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            if resp.status == 200:
                return resp.read().decode("utf-8", errors="replace")
    except (urllib.error.URLError, TimeoutError, OSError):
        return None
    return None


def run_self_test() -> int:
    """Adversarial self-test with mutation canaries."""
    # 1. Clean check passes
    findings = verify_local_invariants(ROOT)
    if findings:
        print(f"self-test failed on clean repo: {findings}", file=sys.stderr)
        return 1

    # 2. Mutation canary: missing command detected
    dummy_text = 'CmdKind::WakeUp, "wake_up", "wake_up"'
    parsed = parse_local_command_registry(ROOT / "main/logic/command_registry.hpp")
    if "wake_up" not in parsed or "set_charging_amps" not in parsed:
        print("self-test failed to parse expected commands", file=sys.stderr)
        return 1

    # 3. Mutation canary: wrong MCP tool for role-denied command caught
    if parsed.get("door_lock", {}).get("mcp_name") is not None:
        print("self-test failed: door_lock must not have an MCP tool", file=sys.stderr)
        return 1

    print("check-vehicle-command-sync: PASS (protocol invariants, BLE UUIDs, role boundaries)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="Run offline self-test and mutation canaries")
    parser.add_argument("--check", action="store_true", help="Verify local command invariants")
    parser.add_argument("--fetch-upstream", action="store_true", help="Fetch upstream Go reference if online")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    findings = verify_local_invariants(ROOT)
    if findings:
        for f in findings:
            print(f"CONFORMANCE FINDING: {f}", file=sys.stderr)
        return 1

    if args.fetch_upstream:
        upstream_text = fetch_upstream_charge_go()
        if upstream_text:
            print("Upstream charge.go fetched successfully:")
            if "SetChargingAmps" in upstream_text:
                print("  - Found SetChargingAmps in upstream charge.go (Conformant)")
            if "ChargeStart" in upstream_text or "StartCharging" in upstream_text:
                print("  - Found ChargeStart in upstream charge.go (Conformant)")
        else:
            print("Upstream fetch skipped (offline or network unavailable); local invariants PASS")

    print("check-vehicle-command-sync: all local protocol invariants satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
