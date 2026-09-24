#!/usr/bin/env python3
"""Render ESP-IDF 6 esp-idf-size `json2` output as a compact Markdown budget report."""

from __future__ import annotations

import argparse
import json
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


# esp-idf-size 2.x (`--format json2`) reports named memory regions. Region names differ per chip
# (the C3 names its shared D/IRAM "DRAM", the S3 and C6 "DIRAM"), so each target's static-RAM and
# IRAM regions are bound explicitly: a renamed or missing region fails closed instead of silently
# reporting zero. Flash regions have no capacity (total == free == 0).
TARGET_REGIONS: dict[str, tuple[str, str, str | None, tuple[str, ...]]] = {
    # target: (memory model, static RAM region, IRAM region, flash regions)
    "esp32": ("split", "DRAM", "IRAM", ("Flash Code", "Flash Data")),
    "esp32s3": ("unified", "DIRAM", "IRAM", ("Flash Code", "Flash Data")),
    "esp32c3": ("unified", "DRAM", None, ("Flash Code", "Flash Data")),
    "esp32c6": ("unified", "DIRAM", None, ("Flash Code",)),
}


def integer(data: dict[str, Any], key: str, label: str) -> int:
    value = data.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{label}.{key} must be a non-negative integer in ESP-IDF size JSON")
    return value


def regions(data: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Validate every json2 region and index it by name."""
    if data.get("version") != "1.2":
        raise ValueError("ESP-IDF size JSON must be esp-idf-size json2 version 1.2")
    layout = data.get("layout")
    if not isinstance(layout, list) or not layout:
        raise ValueError("ESP-IDF size JSON layout must be a non-empty list")
    indexed: dict[str, dict[str, Any]] = {}
    for region in layout:
        if not isinstance(region, dict) or set(region) != {"name", "total", "used", "free", "parts"}:
            raise ValueError("ESP-IDF size region fields must be exactly name/total/used/free/parts")
        name = region["name"]
        if not isinstance(name, str) or not name or name in indexed:
            raise ValueError(f"ESP-IDF size region name is invalid or duplicated: {name!r}")
        total = integer(region, "total", name)
        used = integer(region, "used", name)
        free = integer(region, "free", name)
        parts = region["parts"]
        if not isinstance(parts, dict):
            raise ValueError(f"{name}.parts must be an object in ESP-IDF size JSON")
        part_sum = 0
        for part_name, part in parts.items():
            if not isinstance(part, dict) or set(part) != {"size"}:
                raise ValueError(f"{name}.parts[{part_name!r}] must be exactly {{size}}")
            part_sum += integer(part, "size", f"{name}.parts[{part_name!r}]")
        if used != part_sum:
            raise ValueError(f"{name}.used must equal the sum of its section sizes")
        if total:
            if used > total:
                raise ValueError(f"{name} used bytes cannot exceed its total")
            if free != total - used:
                raise ValueError(f"{name}.free must equal total - used")
        elif free:
            raise ValueError(f"{name} has no capacity, so free must be 0")
        indexed[name] = region
    integer(data, "total_size", "size report")
    return indexed


def target_region(indexed: dict[str, dict[str, Any]], name: str, target: str) -> dict[str, Any]:
    region = indexed.get(name)
    if region is None:
        raise ValueError(f"{target} size report has no {name!r} region")
    return region


def part_size(region: dict[str, Any], part: str) -> int:
    entry = region["parts"].get(part)
    return 0 if entry is None else entry["size"]


def kib(value: int) -> str:
    return f"{value / 1024:.1f} KiB"


def memory_usage(data: dict[str, Any], target: str) -> MemoryUsage:
    if target not in TARGET_REGIONS:
        raise ValueError(f"unsupported target: {target}")
    model, static_name, iram_name, _flash = TARGET_REGIONS[target]
    indexed = regions(data)
    static = target_region(indexed, static_name, target)
    iram = target_region(indexed, iram_name, target) if iram_name else None
    if model == "split" and part_size(static, ".text"):
        raise ValueError(f"{target} split {static_name} region must not hold code")
    usage = MemoryUsage(
        model,
        static["used"],
        static["total"],
        part_size(static, ".bss"),
        iram["used"] if iram else 0,
        iram["total"] if iram else 0,
    )
    if not 0 <= usage.bss <= usage.static_used <= usage.static_capacity or not usage.static_capacity:
        raise ValueError(
            "static memory values must satisfy 0 <= bss <= used <= capacity (capacity > 0) "
            f"(got {usage.bss}, {usage.static_used}, {usage.static_capacity})"
        )
    if not 0 <= usage.iram_used <= usage.iram_capacity:
        raise ValueError(
            "IRAM values must satisfy 0 <= used <= capacity "
            f"(got {usage.iram_used}, {usage.iram_capacity})"
        )
    return usage


def image_usage(data: dict[str, Any], target: str, unsigned_size: int) -> ImageUsage:
    if target not in TARGET_REGIONS:
        raise ValueError(f"unsupported target: {target}")
    indexed = regions(data)
    if isinstance(unsigned_size, bool) or not isinstance(unsigned_size, int) or unsigned_size <= 0:
        raise ValueError("unsigned app size must be a positive integer")
    flash_code_rodata = 0
    for name in TARGET_REGIONS[target][3]:
        region = target_region(indexed, name, target)
        if region["total"]:
            raise ValueError(f"{target} {name} region must not report a capacity")
        flash_code_rodata += region["used"]
    total = data["total_size"]
    if flash_code_rodata > total:
        raise ValueError(
            "ESP-IDF total_size must include every flash region: "
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
    if root.get("toolchain") != "ESP-IDF v6.1":
        raise ValueError("firmware size baseline must be bound to ESP-IDF v6.1")
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
    data: dict[str, Any], target: str, unsigned_size: int, budget: dict[str, Any]
) -> list[str]:
    memory = memory_usage(data, target)
    image = image_usage(data, target, unsigned_size)
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
    image = image_usage(data, target, unsigned_size)
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
    usage = memory_usage(data, target)
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
            "performed later in a trusted job; the JSON artifact retains the raw json2 regions.",
            "Raw image and static-memory baseline growth is fail-closed and requires an explicit reviewed baseline update.",
            "",
        ]
    )
    return "\n".join(rows)


def region(name: str, total: int, parts: dict[str, int]) -> dict[str, Any]:
    used = sum(parts.values())
    return {
        "name": name,
        "total": total,
        "used": used,
        "free": total - used if total else 0,
        "parts": {part: {"size": size} for part, size in parts.items()},
    }


def size_report(total_size: int, *layout: dict[str, Any]) -> dict[str, Any]:
    return {"version": "1.2", "total_size": total_size, "layout": list(layout)}


def with_region(data: dict[str, Any], name: str, **changes: Any) -> dict[str, Any]:
    copy = json.loads(json.dumps(data))
    for entry in copy["layout"]:
        if entry["name"] == name:
            entry.update(changes)
            return copy
    raise AssertionError(f"self-test fixture has no {name} region")


def self_test() -> None:
    # esp32-shaped split report: code in IRAM, data/bss in DRAM, two flash regions.
    fixture = size_report(
        173000,
        region("Flash Code", 0, {".text": 120000}),
        region("Flash Data", 0, {".rodata": 36000, ".eh_frame": 4000}),
        region("IRAM", 50000, {".text": 19000, ".vectors": 1000}),
        region("DRAM", 100000, {".bss": 12000, ".data": 17000, ".noinit": 1000}),
    )
    report = render(fixture, 180000, 200704, 204800, "esp32")
    assert "Projected signed app (PASS)" in report
    assert "4.0 KiB" in report
    assert "DRAM `.bss` | 11.7 KiB" in report
    assert "Flash code + rodata | 156.2 KiB" in report
    # esp32c6-shaped unified report: one flash region, shared D/IRAM named DIRAM.
    unified = size_report(
        173000,
        region("Flash Code", 0, {".text": 120000, ".rodata": 40000}),
        region("DIRAM", 300000, {".text": 90000, ".bss": 45000, ".data": 14000, ".noinit": 1000}),
        region("LP SRAM", 16384, {".rtc_reserved": 24}),
    )
    unified_report = render(unified, 180000, 200704, 204800, "esp32c6")
    assert "Unified D/IRAM | 146.5 KiB" in unified_report
    assert "| DRAM |" not in unified_report
    # esp32c3 names its shared D/IRAM "DRAM"; it is still the unified model.
    c3 = size_report(
        173000,
        region("Flash Code", 0, {".text": 120000}),
        region("Flash Data", 0, {".rodata": 40000}),
        region("DRAM", 300000, {".text": 90000, ".bss": 45000, ".data": 15000}),
    )
    assert memory_usage(c3, "esp32c3").model == "unified"
    failed = render(fixture, 180000, 200704, 200000, "esp32")
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
    assert budget_failures(fixture, "esp32", 180000, budget) == []
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
    assert budget_failures(only_elf_grown, "esp32", 180000, budget) == [
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
    changed_capacity = with_region(fixture, "DRAM", total=100001, free=70001)
    changed_capacity_report = render(
        changed_capacity, 180000, 200704, 204800, "esp32", budget
    )
    assert "Static RAM capacity identity (FAIL)" in changed_capacity_report
    assert "Static RAM reviewed baseline (PASS)" in changed_capacity_report
    grown = with_region(
        fixture, "DRAM", used=30001, free=69999,
        parts={".bss": {"size": 12001}, ".data": {"size": 17000}, ".noinit": {"size": 1000}},
    )
    assert any(
        "static .bss grew" in failure
        for failure in budget_failures(grown, "esp32", 180000, budget)
    )

    # Growth mutation canaries for the three raw-image dimensions. These remain deliberately
    # separate from the projected-signed/slot policy: growth inside one 64 KiB signing bucket must
    # still require review even though the hard slot gate would continue to pass.
    growth_mutations = (
        (fixture, 180001, "unsigned app binary grew"),
        (dict(fixture, total_size=173001), 180000, "ELF image footprint grew"),
        (
            with_region(fixture, "Flash Code", used=120001, parts={".text": {"size": 120001}}),
            180000,
            "flash code + rodata grew",
        ),
    )
    for mutated, mutated_unsigned, expected_message in growth_mutations:
        failures = budget_failures(mutated, "esp32", mutated_unsigned, budget)
        assert any(expected_message in failure for failure in failures), (
            f"growth mutation escaped reviewed baseline: {expected_message}"
        )

    # Every raw json2 number must be a non-negative integer before it reaches a baseline.
    for name in ("Flash Code", "IRAM", "DRAM"):
        for key in ("total", "used", "free"):
            for invalid_value in (-1, 0.5, True):
                invalid = with_region(fixture, name, **{key: invalid_value})
                try:
                    budget_failures(invalid, "esp32", 180000, budget)
                except ValueError as exc:
                    assert f"{name}.{key}" in str(exc), exc
                else:
                    raise AssertionError(
                        f"invalid raw IDF size field reached baseline comparison: "
                        f"{name}.{key}={invalid_value!r}"
                    )
    for invalid_value in (-1, 0.5, True):
        try:
            budget_failures(dict(fixture, total_size=invalid_value), "esp32", 180000, budget)
        except ValueError as exc:
            assert "total_size" in str(exc)
        else:
            raise AssertionError(f"invalid total_size reached baseline: {invalid_value!r}")

    raw_identity_canaries = (
        (with_region(fixture, "DRAM", used=30001, free=69999), "DRAM.used must equal"),
        (with_region(fixture, "IRAM", used=20001, free=29999), "IRAM.used must equal"),
        (with_region(unified, "DIRAM", used=150001, free=149999), "DIRAM.used must equal"),
        (with_region(fixture, "DRAM", free=70001), "DRAM.free must equal"),
        (with_region(fixture, "Flash Data", free=1), "free must be 0"),
        (with_region(fixture, "Flash Code", total=1000000, free=880000), "must not report a capacity"),
        (dict(fixture, version="1.1"), "json2 version 1.2"),
        (dict(fixture, layout=fixture["layout"][:3]), "no 'DRAM' region"),
        (dict(fixture, layout=fixture["layout"] + [fixture["layout"][0]]), "duplicated"),
        (with_region(fixture, "DRAM", used=129000, free=-1,
                     parts={".bss": {"size": 12000}, ".data": {"size": 17000},
                            ".noinit": {"size": 1000}, ".text": {"size": 99000}}),
         "DRAM.free"),
    )
    for invalid, expected_message in raw_identity_canaries:
        try:
            budget_failures(invalid, "esp32", 180000, budget)
        except ValueError as exc:
            assert expected_message in str(exc), exc
        else:
            raise AssertionError(
                f"invalid raw IDF size field reached baseline comparison: {expected_message}"
            )
    split_with_code = with_region(
        fixture, "DRAM", used=31000, free=69000,
        parts={".bss": {"size": 12000}, ".data": {"size": 17000}, ".noinit": {"size": 1000},
               ".text": {"size": 1000}},
    )
    try:
        memory_usage(split_with_code, "esp32")
    except ValueError as exc:
        assert "must not hold code" in str(exc)
    else:
        raise AssertionError("split DRAM holding code was accepted")

    invalid_fixtures = (
        (
            with_region(fixture, "DRAM", used=101000, free=0,
                        parts={".bss": {"size": 12000}, ".data": {"size": 88000},
                               ".noinit": {"size": 1000}}),
            "cannot exceed",
        ),
        (dict(fixture, total_size=159999), "must include every flash region"),
    )
    for invalid, expected_message in invalid_fixtures:
        try:
            render(invalid, 180000, 200704, 204800, "esp32")
        except ValueError as exc:
            assert expected_message in str(exc), exc
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
        assert image_usage(json.loads(path.read_text(encoding="utf-8")), "esp32", 180000).elf_total == 173000
        baseline = pathlib.Path(temp_dir) / "baseline.json"
        targets = {target: dict(budget) for target in TARGETS}
        baseline.write_text(
            json.dumps(
                {
                    "schemaVersion": 2,
                    "baselineKind": "reviewed-maxima",
                    "toolchain": "ESP-IDF v6.1",
                    "targets": targets,
                }
            ),
            encoding="utf-8",
        )
        assert load_budget(baseline, "esp32") == budget
        wrong_toolchain = json.loads(baseline.read_text(encoding="utf-8"))
        wrong_toolchain["toolchain"] = "ESP-IDF v5.5.5"
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
                    "toolchain": "ESP-IDF v6.1",
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
        failures = budget_failures(data, args.target, unsigned_size, budget)
        if failures:
            for failure in failures:
                print(f"firmware size baseline failed: {failure}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
