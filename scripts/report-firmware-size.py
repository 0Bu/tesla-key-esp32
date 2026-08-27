#!/usr/bin/env python3
"""Render ESP-IDF 5.x legacy JSON size output as a compact Markdown budget report."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
import tempfile
from dataclasses import dataclass
from typing import Any


TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
SIGNATURE_ALIGNMENT = 0x10000
SIGNATURE_SECTOR = 0x1000


@dataclass(frozen=True)
class MemoryUsage:
    model: str
    static_used: int
    static_capacity: int
    bss: int
    iram_used: int
    iram_capacity: int


@dataclass(frozen=True)
class ImageUsage:
    unsigned_app: int
    elf_total: int
    flash_code_rodata: int


def integer(data: dict[str, Any], key: str) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{key} must be an integer in ESP-IDF size JSON")
    return value


def non_negative_integer(data: dict[str, Any], key: str) -> int:
    value = integer(data, key)
    if value < 0:
        raise ValueError(f"{key} must be a non-negative integer in ESP-IDF size JSON")
    return value


def ratio(data: dict[str, Any], key: str) -> float:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{key} must be a finite non-negative number in ESP-IDF size JSON")
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise ValueError(f"{key} must be a finite non-negative number in ESP-IDF size JSON")
    return result


def validate_region(
    data: dict[str, Any],
    name: str,
    part_keys: tuple[str, ...],
    used_key: str,
    total_key: str,
    remain_key: str,
    ratio_key: str,
) -> tuple[int, int]:
    parts = [non_negative_integer(data, key) for key in part_keys]
    used = non_negative_integer(data, used_key)
    total = non_negative_integer(data, total_key)
    remain = non_negative_integer(data, remain_key)
    reported_ratio = ratio(data, ratio_key)
    part_sum = sum(parts)
    if used != part_sum:
        raise ValueError(f"{used_key} must equal the sum of {name} section fields")
    if used > total:
        raise ValueError(f"{name} used bytes cannot exceed {total_key}")
    if remain != total - used:
        raise ValueError(f"{remain_key} must equal {total_key} - {used_key}")
    expected_ratio = used / total if total else 0.0
    if not math.isclose(reported_ratio, expected_ratio, rel_tol=1e-12, abs_tol=1e-12):
        raise ValueError(f"{ratio_key} does not match {used_key} / {total_key}")
    return used, total


def validate_idf_size_json(data: dict[str, Any]) -> None:
    dram_used, dram_total = validate_region(
        data,
        "DRAM",
        ("dram_data", "dram_bss", "dram_rodata", "dram_other"),
        "used_dram",
        "dram_total",
        "dram_remain",
        "used_dram_ratio",
    )
    validate_region(
        data,
        "IRAM",
        ("iram_vectors", "iram_text", "iram_other"),
        "used_iram",
        "iram_total",
        "iram_remain",
        "used_iram_ratio",
    )
    _, diram_total = validate_region(
        data,
        "D/IRAM",
        (
            "diram_data",
            "diram_bss",
            "diram_text",
            "diram_vectors",
            "diram_rodata",
            "diram_other",
        ),
        "used_diram",
        "diram_total",
        "diram_remain",
        "used_diram_ratio",
    )
    if diram_total and (dram_used or dram_total):
        raise ValueError("unified D/IRAM reports must not also claim a split DRAM region")

    flash_code = non_negative_integer(data, "flash_code")
    flash_rodata = non_negative_integer(data, "flash_rodata")
    flash_other = non_negative_integer(data, "flash_other")
    used_flash = non_negative_integer(data, "used_flash_non_ram")
    if used_flash != flash_code + flash_rodata + flash_other:
        raise ValueError(
            "used_flash_non_ram must equal flash_code + flash_rodata + flash_other"
        )
    total_size = non_negative_integer(data, "total_size")
    if used_flash > total_size:
        raise ValueError("used_flash_non_ram cannot exceed total_size")


def kib(value: int) -> str:
    return f"{value / 1024:.1f} KiB"


def memory_usage(data: dict[str, Any]) -> MemoryUsage:
    validate_idf_size_json(data)
    diram_total = integer(data, "diram_total")
    iram_total = integer(data, "iram_total")
    if diram_total:
        usage = MemoryUsage(
            "unified",
            integer(data, "used_diram"),
            diram_total,
            integer(data, "diram_bss"),
            integer(data, "used_iram"),
            iram_total,
        )
    else:
        usage = MemoryUsage(
            "split",
            integer(data, "used_dram"),
            integer(data, "dram_total"),
            integer(data, "dram_bss"),
            integer(data, "used_iram"),
            iram_total,
        )
    if not 0 <= usage.bss <= usage.static_used <= usage.static_capacity:
        raise ValueError(
            "static memory values must satisfy 0 <= bss <= used <= capacity "
            f"(got {usage.bss}, {usage.static_used}, {usage.static_capacity})"
        )
    if not 0 <= usage.iram_used <= usage.iram_capacity:
        raise ValueError(
            "IRAM values must satisfy 0 <= used <= capacity "
            f"(got {usage.iram_used}, {usage.iram_capacity})"
        )
    return usage


def image_usage(data: dict[str, Any], unsigned_size: int) -> ImageUsage:
    validate_idf_size_json(data)
    if isinstance(unsigned_size, bool) or not isinstance(unsigned_size, int) or unsigned_size <= 0:
        raise ValueError("unsigned app size must be a positive integer")
    total = integer(data, "total_size")
    flash_code = integer(data, "flash_code")
    flash_rodata = integer(data, "flash_rodata")
    if min(total, flash_code, flash_rodata) < 0:
        raise ValueError("ELF total, flash code and flash rodata sizes must be non-negative")
    flash_code_rodata = flash_code + flash_rodata
    if flash_code_rodata > total:
        raise ValueError(
            "ESP-IDF total_size must include flash_code + flash_rodata: "
            f"total={total} flash={flash_code_rodata}"
        )
    if total > unsigned_size:
        raise ValueError(
            "ESP-IDF total_size cannot exceed the emitted unsigned app binary: "
            f"total={total} app={unsigned_size}"
        )
    return ImageUsage(unsigned_size, total, flash_code_rodata)


def load_budget(path: pathlib.Path, target: str) -> dict[str, Any]:
    root = json.loads(path.read_text(encoding="utf-8"))
    expected_top = {"schemaVersion", "baselineKind", "toolchain", "targets"}
    if not isinstance(root, dict) or set(root) != expected_top:
        raise ValueError(f"firmware size baseline fields must be exactly {sorted(expected_top)}")
    if root.get("schemaVersion") != 2 or root.get("baselineKind") != "reviewed-maxima":
        raise ValueError("firmware size baseline must be schemaVersion 2 reviewed-maxima")
    if root.get("toolchain") != "ESP-IDF v5.5.5":
        raise ValueError("firmware size baseline must be bound to ESP-IDF v5.5.5")
    targets = root.get("targets")
    if not isinstance(targets, dict) or set(targets) != set(TARGETS):
        raise ValueError("firmware size baseline must contain exactly the four supported targets")
    budget = targets.get(target)
    expected = {
        "memoryModel",
        "staticCapacity",
        "maxStaticUsed",
        "maxBss",
        "iramCapacity",
        "maxIramUsed",
        "maxUnsignedApp",
        "maxElfTotal",
        "maxFlashCodeAndRodata",
    }
    if not isinstance(budget, dict) or set(budget) != expected:
        raise ValueError(f"firmware size baseline for {target} has invalid fields")
    if budget["memoryModel"] not in {"split", "unified"}:
        raise ValueError(f"firmware size baseline for {target} has invalid memoryModel")
    for key in expected - {"memoryModel"}:
        value = budget[key]
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise ValueError(f"firmware size baseline {target}.{key} must be a non-negative integer")
    if not 0 <= budget["maxBss"] <= budget["maxStaticUsed"] <= budget["staticCapacity"]:
        raise ValueError(
            f"firmware size baseline {target} must satisfy maxBss <= maxStaticUsed <= staticCapacity"
        )
    if not 0 <= budget["maxIramUsed"] <= budget["iramCapacity"]:
        raise ValueError(
            f"firmware size baseline {target} must satisfy maxIramUsed <= iramCapacity"
        )
    if not (
        0
        < budget["maxFlashCodeAndRodata"]
        <= budget["maxElfTotal"]
        <= budget["maxUnsignedApp"]
    ):
        raise ValueError(
            f"firmware size baseline {target} must satisfy "
            "0 < maxFlashCodeAndRodata <= maxElfTotal <= maxUnsignedApp"
        )
    return budget


def budget_failures(
    data: dict[str, Any], unsigned_size: int, budget: dict[str, Any]
) -> list[str]:
    memory = memory_usage(data)
    image = image_usage(data, unsigned_size)
    failures: list[str] = []
    if memory.model != budget["memoryModel"]:
        failures.append(f"memory model changed from {budget['memoryModel']} to {memory.model}")
    for label, actual, key in (
        ("static RAM capacity", memory.static_capacity, "staticCapacity"),
        ("IRAM capacity", memory.iram_capacity, "iramCapacity"),
    ):
        if actual != budget[key]:
            failures.append(f"{label} changed: baseline={budget[key]} actual={actual}")
    for label, actual, key in (
        ("unsigned app binary", image.unsigned_app, "maxUnsignedApp"),
        ("ELF image footprint", image.elf_total, "maxElfTotal"),
        ("flash code + rodata", image.flash_code_rodata, "maxFlashCodeAndRodata"),
        ("static RAM used", memory.static_used, "maxStaticUsed"),
        ("static .bss", memory.bss, "maxBss"),
        ("IRAM used", memory.iram_used, "maxIramUsed"),
    ):
        if actual > budget[key]:
            failures.append(f"{label} grew beyond reviewed baseline: max={budget[key]} actual={actual}")
    return failures


def render(
    data: dict[str, Any],
    unsigned_size: int,
    projected_signed_size: int,
    policy_limit: int,
    target: str,
    budget: dict[str, Any] | None = None,
) -> str:
    if min(unsigned_size, projected_signed_size, policy_limit) <= 0:
        raise ValueError("binary sizes and policy limit must be positive")
    expected_signed_size = (
        (unsigned_size + SIGNATURE_ALIGNMENT - 1) // SIGNATURE_ALIGNMENT
        * SIGNATURE_ALIGNMENT
        + SIGNATURE_SECTOR
    )
    if projected_signed_size != expected_signed_size:
        raise ValueError(
            "projected signed size must exactly equal minimal Secure Boot v2 padding plus its "
            f"signature sector: expected {expected_signed_size}, got {projected_signed_size}"
        )
    image = image_usage(data, unsigned_size)
    margin = policy_limit - projected_signed_size
    state = "PASS" if margin >= 0 else "FAIL"
    rows = [
        f"## Firmware size — {target}",
        "",
        "| Metric | Used | Capacity / policy | Free |",
        "|---|---:|---:|---:|",
        f"| Projected signed app ({state}) | {kib(projected_signed_size)} | {kib(policy_limit)} | {kib(margin)} |",
        f"| Unsigned app binary | {kib(image.unsigned_app)} | — | — |",
        f"| ELF image footprint | {kib(image.elf_total)} | — | — |",
    ]
    # Xtensa ESP32 reports split DRAM/IRAM, while S3 and RISC-V targets primarily report a
    # unified D/IRAM region. Never print plausible-looking zero rows for the wrong memory model.
    usage = memory_usage(data)
    if usage.model == "unified":
        rows.extend(
            [
                f"| Unified D/IRAM | {kib(usage.static_used)} | {kib(usage.static_capacity)} | {kib(usage.static_capacity - usage.static_used)} |",
                f"| Unified D/IRAM `.bss` | {kib(usage.bss)} | — | — |",
            ]
        )
    else:
        rows.extend(
            [
                f"| DRAM | {kib(usage.static_used)} | {kib(usage.static_capacity)} | {kib(usage.static_capacity - usage.static_used)} |",
                f"| DRAM `.bss` | {kib(usage.bss)} | — | — |",
            ]
        )
    if usage.iram_capacity:
        rows.append(
            f"| IRAM | {kib(usage.iram_used)} | {kib(usage.iram_capacity)} | "
            f"{kib(usage.iram_capacity - usage.iram_used)} |"
        )
    if budget is not None:
        model_state = "PASS" if usage.model == budget["memoryModel"] else "FAIL"
        static_capacity_state = (
            "PASS" if usage.static_capacity == budget["staticCapacity"] else "FAIL"
        )
        iram_capacity_state = (
            "PASS" if usage.iram_capacity == budget["iramCapacity"] else "FAIL"
        )

        def maximum_state(actual: int, key: str) -> str:
            return "PASS" if actual <= budget[key] else "FAIL"

        rows.extend(
            [
                f"| Memory model identity ({model_state}) | {usage.model} | {budget['memoryModel']} | — |",
                f"| Static RAM capacity identity ({static_capacity_state}) | {kib(usage.static_capacity)} | {kib(budget['staticCapacity'])} | {kib(budget['staticCapacity'] - usage.static_capacity)} |",
                f"| IRAM capacity identity ({iram_capacity_state}) | {kib(usage.iram_capacity)} | {kib(budget['iramCapacity'])} | {kib(budget['iramCapacity'] - usage.iram_capacity)} |",
                f"| Unsigned app reviewed baseline ({maximum_state(image.unsigned_app, 'maxUnsignedApp')}) | {kib(image.unsigned_app)} | {kib(budget['maxUnsignedApp'])} | {kib(budget['maxUnsignedApp'] - image.unsigned_app)} |",
                f"| ELF total reviewed baseline ({maximum_state(image.elf_total, 'maxElfTotal')}) | {kib(image.elf_total)} | {kib(budget['maxElfTotal'])} | {kib(budget['maxElfTotal'] - image.elf_total)} |",
                f"| Flash code + rodata reviewed baseline ({maximum_state(image.flash_code_rodata, 'maxFlashCodeAndRodata')}) | {kib(image.flash_code_rodata)} | {kib(budget['maxFlashCodeAndRodata'])} | {kib(budget['maxFlashCodeAndRodata'] - image.flash_code_rodata)} |",
                f"| Static RAM reviewed baseline ({maximum_state(usage.static_used, 'maxStaticUsed')}) | {kib(usage.static_used)} | {kib(budget['maxStaticUsed'])} | {kib(budget['maxStaticUsed'] - usage.static_used)} |",
                f"| Static `.bss` reviewed baseline ({maximum_state(usage.bss, 'maxBss')}) | {kib(usage.bss)} | {kib(budget['maxBss'])} | {kib(budget['maxBss'] - usage.bss)} |",
                f"| IRAM reviewed baseline ({maximum_state(usage.iram_used, 'maxIramUsed')}) | {kib(usage.iram_used)} | {kib(budget['maxIramUsed'])} | {kib(budget['maxIramUsed'] - usage.iram_used)} |",
            ]
        )
    rows.extend(
        [
            f"| Flash code + rodata | {kib(image.flash_code_rodata)} | — | — |",
            "",
            "The policy row projects Secure Boot v2 padding plus its signature sector. Signing is",
            "performed later in a trusted job; the JSON artifact retains the IDF 5.x raw fields.",
            "Raw image and static-memory baseline growth is fail-closed and requires an explicit reviewed baseline update.",
            "",
        ]
    )
    return "\n".join(rows)


def self_test() -> None:
    fixture = {
        "dram_data": 1000,
        "dram_bss": 12000,
        "dram_rodata": 16000,
        "dram_other": 1000,
        "used_dram": 30000,
        "dram_total": 100000,
        "used_dram_ratio": 0.3,
        "dram_remain": 70000,
        "iram_vectors": 1000,
        "iram_text": 18000,
        "iram_other": 1000,
        "used_iram": 20000,
        "iram_total": 50000,
        "used_iram_ratio": 0.4,
        "iram_remain": 30000,
        "diram_data": 0,
        "diram_bss": 0,
        "diram_text": 0,
        "diram_vectors": 0,
        "diram_rodata": 0,
        "diram_other": 0,
        "diram_total": 0,
        "used_diram": 0,
        "used_diram_ratio": 0,
        "diram_remain": 0,
        "flash_code": 120000,
        "flash_rodata": 40000,
        "flash_other": 5000,
        "used_flash_non_ram": 165000,
        "total_size": 173000,
    }
    report = render(fixture, 180000, 200704, 204800, "esp32c6")
    assert "Projected signed app (PASS)" in report
    assert "4.0 KiB" in report
    assert "DRAM `.bss` | 11.7 KiB" in report
    assert "Flash code + rodata | 156.2 KiB" in report
    unified = dict(
        fixture,
        dram_data=0,
        dram_bss=0,
        dram_rodata=0,
        dram_other=0,
        used_dram=0,
        dram_total=0,
        used_dram_ratio=0,
        dram_remain=0,
        iram_vectors=0,
        iram_text=0,
        iram_other=0,
        used_iram=0,
        iram_total=0,
        used_iram_ratio=0,
        iram_remain=0,
        diram_data=10000,
        diram_bss=45000,
        diram_text=90000,
        diram_vectors=1000,
        diram_rodata=3000,
        diram_other=1000,
        diram_total=300000,
        used_diram=150000,
        used_diram_ratio=0.5,
        diram_remain=150000,
    )
    unified_report = render(unified, 180000, 200704, 204800, "esp32c6")
    assert "Unified D/IRAM | 146.5 KiB" in unified_report
    assert "| DRAM |" not in unified_report
    failed = render(fixture, 180000, 200704, 200000, "esp32c6")
    assert "Projected signed app (FAIL)" in failed

    budget = {
        "memoryModel": "split",
        "staticCapacity": 100000,
        "maxStaticUsed": 30000,
        "maxBss": 12000,
        "iramCapacity": 50000,
        "maxIramUsed": 20000,
        "maxUnsignedApp": 180000,
        "maxElfTotal": 173000,
        "maxFlashCodeAndRodata": 160000,
    }
    assert budget_failures(fixture, 180000, budget) == []
    assert "Static RAM reviewed baseline (PASS)" in render(
        fixture, 180000, 200704, 204800, "esp32", budget
    )
    assert "Unsigned app reviewed baseline (PASS)" in render(
        fixture, 180000, 200704, 204800, "esp32", budget
    )
    only_elf_grown = dict(fixture, total_size=173001)
    only_elf_report = render(
        only_elf_grown, 180000, 200704, 204800, "esp32", budget
    )
    assert "ELF total reviewed baseline (FAIL)" in only_elf_report
    assert budget_failures(only_elf_grown, 180000, budget) == [
        "ELF image footprint grew beyond reviewed baseline: max=173000 actual=173001"
    ]
    for unaffected in (
        "Unsigned app reviewed baseline (PASS)",
        "Flash code + rodata reviewed baseline (PASS)",
        "Static RAM reviewed baseline (PASS)",
        "Static `.bss` reviewed baseline (PASS)",
        "IRAM reviewed baseline (PASS)",
    ):
        assert unaffected in only_elf_report, (
            f"unrelated baseline row inherited ELF failure: {unaffected}"
        )
    changed_capacity = dict(
        fixture,
        dram_total=100001,
        dram_remain=70001,
        used_dram_ratio=30000 / 100001,
    )
    changed_capacity_report = render(
        changed_capacity, 180000, 200704, 204800, "esp32", budget
    )
    assert "Static RAM capacity identity (FAIL)" in changed_capacity_report
    assert "Static RAM reviewed baseline (PASS)" in changed_capacity_report
    grown = dict(
        fixture,
        dram_bss=12001,
        used_dram=30001,
        dram_remain=69999,
        used_dram_ratio=30001 / 100000,
    )
    assert any(
        "static .bss grew" in failure
        for failure in budget_failures(grown, 180000, budget)
    )

    # Growth mutation canaries for the three raw-image dimensions. These remain deliberately
    # separate from the projected-signed/slot policy: growth inside one 64 KiB signing bucket must
    # still require review even though the hard slot gate would continue to pass.
    growth_mutations = (
        (fixture, 180001, "unsigned app binary grew"),
        (dict(fixture, total_size=173001), 180000, "ELF image footprint grew"),
        (
            dict(fixture, flash_code=120001, used_flash_non_ram=165001),
            180000,
            "flash code + rodata grew",
        ),
    )
    for mutated, mutated_unsigned, expected_message in growth_mutations:
        failures = budget_failures(mutated, mutated_unsigned, budget)
        assert any(expected_message in failure for failure in failures), (
            f"growth mutation escaped reviewed baseline: {expected_message}"
        )

    raw_integer_fields = (
        "dram_data",
        "dram_bss",
        "dram_rodata",
        "dram_other",
        "used_dram",
        "dram_total",
        "dram_remain",
        "iram_vectors",
        "iram_text",
        "iram_other",
        "used_iram",
        "iram_total",
        "iram_remain",
        "diram_data",
        "diram_bss",
        "diram_text",
        "diram_vectors",
        "diram_rodata",
        "diram_other",
        "used_diram",
        "diram_total",
        "diram_remain",
        "flash_code",
        "flash_rodata",
        "flash_other",
        "used_flash_non_ram",
        "total_size",
    )
    for key in raw_integer_fields:
        source = unified if key.startswith("diram_") or key == "used_diram" else fixture
        for invalid_value in (-1, 0.5, True):
            invalid = dict(source, **{key: invalid_value})
            try:
                budget_failures(invalid, 180000, budget)
            except ValueError as exc:
                assert key in str(exc)
            else:
                raise AssertionError(
                    f"invalid raw IDF size field reached baseline comparison: "
                    f"{key}={invalid_value!r}"
                )

    raw_identity_canaries = (
        (dict(fixture, used_dram=30001), "used_dram must equal"),
        (dict(fixture, used_iram=20001), "used_iram must equal"),
        (dict(unified, used_diram=150001), "used_diram must equal"),
        (dict(fixture, used_flash_non_ram=165001), "used_flash_non_ram must equal"),
        (dict(fixture, dram_remain=70001), "dram_remain"),
        (dict(fixture, iram_remain=30001), "iram_remain"),
        (dict(unified, diram_remain=150001), "diram_remain"),
        (dict(fixture, used_dram_ratio=0.31), "used_dram_ratio"),
    )
    for invalid, expected_message in raw_identity_canaries:
        try:
            budget_failures(invalid, 180000, budget)
        except ValueError as exc:
            assert expected_message in str(exc)
        else:
            raise AssertionError(
                f"invalid raw IDF size field reached baseline comparison: {expected_message}"
            )

    invalid_fixtures = (
        (
            dict(
                fixture,
                dram_rodata=87000,
                used_dram=101000,
                dram_remain=0,
                used_dram_ratio=1.01,
            ),
            "cannot exceed",
        ),
        (dict(fixture, total_size=-1), "non-negative"),
        (dict(fixture, total_size=159999), "used_flash_non_ram"),
    )
    for invalid, expected_message in invalid_fixtures:
        try:
            render(invalid, 180000, 200704, 204800, "esp32")
        except ValueError as exc:
            assert expected_message in str(exc)
        else:
            raise AssertionError(f"physically impossible size fixture was accepted: {invalid}")
    try:
        render(fixture, 180000, 196608, 204800, "esp32")
    except ValueError as exc:
        assert "exactly equal" in str(exc)
    else:
        raise AssertionError("inexact Secure Boot v2 signed-size projection was accepted")
    try:
        render(fixture, 160000, 200704, 204800, "esp32")
    except ValueError as exc:
        assert "cannot exceed" in str(exc)
    else:
        raise AssertionError("ELF total larger than emitted app binary was accepted")

    with tempfile.TemporaryDirectory() as temp_dir:
        path = pathlib.Path(temp_dir) / "size.json"
        path.write_text(json.dumps(fixture), encoding="utf-8")
        assert integer(json.loads(path.read_text(encoding="utf-8")), "total_size") == 173000
        baseline = pathlib.Path(temp_dir) / "baseline.json"
        targets = {target: dict(budget) for target in TARGETS}
        baseline.write_text(
            json.dumps(
                {
                    "schemaVersion": 2,
                    "baselineKind": "reviewed-maxima",
                    "toolchain": "ESP-IDF v5.5.5",
                    "targets": targets,
                }
            ),
            encoding="utf-8",
        )
        assert load_budget(baseline, "esp32") == budget
        wrong_toolchain = json.loads(baseline.read_text(encoding="utf-8"))
        wrong_toolchain["toolchain"] = "ESP-IDF v6.0"
        baseline.write_text(json.dumps(wrong_toolchain), encoding="utf-8")
        try:
            load_budget(baseline, "esp32")
        except ValueError:
            pass
        else:
            raise AssertionError("wrong baseline toolchain was accepted")
        misleading_provenance = json.loads(
            json.dumps(
                {
                    "schemaVersion": 2,
                    "baselineKind": "reviewed-maxima",
                    "toolchain": "ESP-IDF v5.5.5",
                    "targets": targets,
                }
            )
        )
        misleading_provenance["sourceSha"] = "0" * 40
        baseline.write_text(json.dumps(misleading_provenance), encoding="utf-8")
        try:
            load_budget(baseline, "esp32")
        except ValueError:
            pass
        else:
            raise AssertionError("unenforced sourceSha baseline field was accepted")
        legacy = json.loads(json.dumps(misleading_provenance))
        legacy.pop("sourceSha")
        legacy["schemaVersion"] = 1
        baseline.write_text(json.dumps(legacy), encoding="utf-8")
        try:
            load_budget(baseline, "esp32")
        except ValueError as exc:
            assert "schemaVersion 2" in str(exc)
        else:
            raise AssertionError("legacy firmware-size baseline schema was accepted")
    print("firmware-size report self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-size", type=pathlib.Path)
    parser.add_argument("--unsigned-app", type=pathlib.Path)
    parser.add_argument("--projected-signed-size", type=int)
    parser.add_argument("--policy-limit", type=int)
    parser.add_argument("--target")
    parser.add_argument("--budget-baseline", type=pathlib.Path)
    parser.add_argument("--enforce-budget", action="store_true")
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
    if args.target not in TARGETS:
        raise ValueError(f"unsupported target: {args.target}")
    if args.enforce_budget and args.budget_baseline is None:
        parser.error("--enforce-budget requires --budget-baseline")
    budget = load_budget(args.budget_baseline, args.target) if args.budget_baseline else None
    unsigned_size = args.unsigned_app.stat().st_size
    print(
        render(
            data,
            unsigned_size,
            args.projected_signed_size,
            args.policy_limit,
            args.target,
            budget,
        ),
        end="",
    )
    if args.projected_signed_size > args.policy_limit:
        print("projected signed application exceeds policy limit", file=sys.stderr)
        return 1
    if args.enforce_budget and budget is not None:
        failures = budget_failures(data, unsigned_size, budget)
        if failures:
            for failure in failures:
                print(f"firmware size baseline failed: {failure}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
