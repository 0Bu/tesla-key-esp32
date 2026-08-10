#!/usr/bin/env python3
"""Render ESP-IDF 5.x legacy JSON size output as a compact Markdown budget report."""

from __future__ import annotations

import argparse
import json
import pathlib
import tempfile
from typing import Any


def integer(data: dict[str, Any], key: str) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{key} must be an integer in ESP-IDF size JSON")
    return value


def kib(value: int) -> str:
    return f"{value / 1024:.1f} KiB"


def render(
    data: dict[str, Any], unsigned_size: int, projected_signed_size: int, policy_limit: int, target: str
) -> str:
    if min(unsigned_size, projected_signed_size, policy_limit) <= 0:
        raise ValueError("binary sizes and policy limit must be positive")
    total = integer(data, "total_size")
    flash_code = integer(data, "flash_code")
    flash_rodata = integer(data, "flash_rodata")
    margin = policy_limit - projected_signed_size
    state = "PASS" if margin >= 0 else "FAIL"
    rows = [
        f"## Firmware size — {target}",
        "",
        "| Metric | Used | Capacity / policy | Free |",
        "|---|---:|---:|---:|",
        f"| Projected signed app ({state}) | {kib(projected_signed_size)} | {kib(policy_limit)} | {kib(margin)} |",
        f"| Unsigned app binary | {kib(unsigned_size)} | — | — |",
        f"| ELF image footprint | {kib(total)} | — | — |",
    ]
    # Xtensa ESP32 reports split DRAM/IRAM, while S3 and RISC-V targets primarily report a
    # unified D/IRAM region. Never print plausible-looking zero rows for the wrong memory model.
    diram_total = integer(data, "diram_total") if "diram_total" in data else 0
    if diram_total:
        used_diram = integer(data, "used_diram")
        rows.extend(
            [
                f"| Unified D/IRAM | {kib(used_diram)} | {kib(diram_total)} | {kib(diram_total - used_diram)} |",
                f"| Unified D/IRAM `.bss` | {kib(integer(data, 'diram_bss'))} | — | — |",
            ]
        )
    else:
        used_dram = integer(data, "used_dram")
        dram_total = integer(data, "dram_total")
        rows.extend(
            [
                f"| DRAM | {kib(used_dram)} | {kib(dram_total)} | {kib(dram_total - used_dram)} |",
                f"| DRAM `.bss` | {kib(integer(data, 'dram_bss'))} | — | — |",
            ]
        )
    iram_total = integer(data, "iram_total")
    if iram_total:
        used_iram = integer(data, "used_iram")
        rows.append(f"| IRAM | {kib(used_iram)} | {kib(iram_total)} | {kib(iram_total - used_iram)} |")
    rows.extend(
        [
            f"| Flash code + rodata | {kib(flash_code + flash_rodata)} | — | — |",
            "",
            "The policy row projects Secure Boot v2 padding plus its signature sector. Signing is",
            "performed later in a trusted job; the JSON artifact retains the IDF 5.x raw fields.",
            "",
        ]
    )
    return "\n".join(rows)


def self_test() -> None:
    fixture = {
        "dram_data": 1000,
        "dram_bss": 12000,
        "used_dram": 30000,
        "dram_total": 100000,
        "diram_total": 0,
        "used_iram": 20000,
        "iram_total": 50000,
        "flash_code": 120000,
        "flash_rodata": 40000,
        "total_size": 173000,
    }
    report = render(fixture, 150000, 196608, 200000, "esp32c6")
    assert "Projected signed app (PASS)" in report
    assert "3.3 KiB" in report
    assert "DRAM `.bss` | 11.7 KiB" in report
    assert "Flash code + rodata | 156.2 KiB" in report
    unified = dict(fixture, diram_total=300000, used_diram=150000, diram_bss=45000, iram_total=0)
    unified_report = render(unified, 150000, 196608, 200000, "esp32c6")
    assert "Unified D/IRAM | 146.5 KiB" in unified_report
    assert "| DRAM |" not in unified_report
    failed = render(fixture, 150000, 200001, 200000, "esp32c6")
    assert "Projected signed app (FAIL)" in failed

    with tempfile.TemporaryDirectory() as temp_dir:
        path = pathlib.Path(temp_dir) / "size.json"
        path.write_text(json.dumps(fixture), encoding="utf-8")
        assert integer(json.loads(path.read_text(encoding="utf-8")), "total_size") == 173000
    print("firmware-size report self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-size", type=pathlib.Path)
    parser.add_argument("--unsigned-app", type=pathlib.Path)
    parser.add_argument("--projected-signed-size", type=int)
    parser.add_argument("--policy-limit", type=int)
    parser.add_argument("--target")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if None in (
        args.idf_size,
        args.unsigned_app,
        args.projected_signed_size,
        args.policy_limit,
        args.target,
    ):
        parser.error("all size inputs are required unless --self-test is used")
    data = json.loads(args.idf_size.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("ESP-IDF size report root must be an object")
    print(
        render(
            data,
            args.unsigned_app.stat().st_size,
            args.projected_signed_size,
            args.policy_limit,
            args.target,
        ),
        end="",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
