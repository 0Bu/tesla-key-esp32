#!/usr/bin/env python3
"""Fail when common or target ESP-IDF defaults did not land in generated sdkconfig."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tempfile


ASSIGNMENT = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
NOT_SET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")
TARGET = re.compile(r"^esp32(?:s3|c3|c6)?$")


class ConfigError(ValueError):
    pass


def parse_config(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or (line.startswith("#") and not NOT_SET.fullmatch(line)):
            continue
        match = ASSIGNMENT.fullmatch(line)
        if match:
            key, value = match.groups()
        else:
            match = NOT_SET.fullmatch(line)
            if match:
                key, value = match.group(1), "n"
            elif line.startswith("CONFIG_"):
                raise ConfigError(f"{path}:{line_no}: malformed CONFIG assignment: {line}")
            else:
                continue
        if key in values:
            raise ConfigError(f"{path}:{line_no}: duplicate assignment for {key}")
        values[key] = value
    return values


def expected_values(common: pathlib.Path, target: str) -> tuple[dict[str, str], list[pathlib.Path]]:
    if not TARGET.fullmatch(target):
        raise ConfigError(f"unsupported target: {target}")
    paths = [common]
    target_path = common.with_name(f"{common.name}.{target}")
    if target_path.exists():
        paths.append(target_path)
    values: dict[str, str] = {}
    for path in paths:
        # Target defaults are applied after the common file and may intentionally override it.
        values.update(parse_config(path))
    return values, paths


def check(common: pathlib.Path, generated: pathlib.Path, target: str) -> tuple[list[str], int]:
    expected, _ = expected_values(common, target)
    actual = parse_config(generated)
    errors: list[str] = []
    target_key = f"CONFIG_IDF_TARGET_{target.upper()}"
    if actual.get(target_key) != "y":
        errors.append(f"{target_key}: generated sdkconfig is not for requested target {target}")
    for key, wanted in expected.items():
        got = actual.get(key)
        if got is None:
            errors.append(f"{key}: missing from generated sdkconfig (default was {wanted})")
        elif got != wanted:
            errors.append(f"{key}: generated {got}, expected {wanted}")
    return errors, len(expected)


def self_test() -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
        root = pathlib.Path(temp_dir)
        common = root / "sdkconfig.defaults"
        target_defaults = root / "sdkconfig.defaults.esp32c3"
        generated = root / "sdkconfig"
        common.write_text(
            "CONFIG_SHARED=y\nCONFIG_OVERRIDE=n\n# CONFIG_OFF is not set\n", encoding="utf-8"
        )
        target_defaults.write_text("CONFIG_TARGET_ONLY=7\nCONFIG_OVERRIDE=y\n", encoding="utf-8")
        generated.write_text(
            "CONFIG_IDF_TARGET_ESP32C3=y\nCONFIG_SHARED=y\nCONFIG_OVERRIDE=y\n"
            "CONFIG_TARGET_ONLY=7\n# CONFIG_OFF is not set\n",
            encoding="utf-8",
        )
        assert check(common, generated, "esp32c3") == ([], 4)

        generated.write_text("CONFIG_IDF_TARGET_ESP32C3=y\nCONFIG_SHARED=n\n", encoding="utf-8")
        errors, _ = check(common, generated, "esp32c3")
        assert any("CONFIG_SHARED: generated n, expected y" in error for error in errors)
        assert any("CONFIG_TARGET_ONLY: missing" in error for error in errors)

        common.write_text("CONFIG_DUP=y\nCONFIG_DUP=n\n", encoding="utf-8")
        try:
            expected_values(common, "esp32c3")
        except ConfigError as error:
            assert "duplicate assignment" in str(error)
        else:
            raise AssertionError("duplicate assignment was accepted")

        common.write_text("CONFIG_BROKEN y\n", encoding="utf-8")
        try:
            expected_values(common, "esp32c3")
        except ConfigError as error:
            assert "malformed CONFIG assignment" in str(error)
        else:
            raise AssertionError("malformed assignment was accepted")

        try:
            expected_values(common, "esp32h2")
        except ConfigError as error:
            assert "unsupported target" in str(error)
        else:
            raise AssertionError("unsupported target was accepted")

    print("sdkconfig-defaults checker self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--defaults", type=pathlib.Path, default=pathlib.Path("sdkconfig.defaults"))
    parser.add_argument("--generated", type=pathlib.Path, default=pathlib.Path("sdkconfig"))
    parser.add_argument("--target", required=False)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.target is None:
        parser.error("--target is required unless --self-test is used")
    try:
        errors, count = check(args.defaults, args.generated, args.target)
    except (ConfigError, OSError) as error:
        print(error, file=sys.stderr)
        return 2
    if errors:
        print(f"sdkconfig defaults drift: {len(errors)} error(s):", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(f"sdkconfig defaults: PASS ({count} assignments match for {args.target})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
