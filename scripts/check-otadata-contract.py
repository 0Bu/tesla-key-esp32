#!/usr/bin/env python3
"""Validate the safe initial ESP-IDF OTA selection partition bytes."""

from __future__ import annotations

import argparse
from pathlib import Path
import stat
import sys
import tempfile


OTADATA_SIZE = 0x2000
ERASED_BYTE = 0xFF


class OtadataError(RuntimeError):
    pass


def validate_bytes(data: bytes, label: str = "ota_data_initial.bin") -> None:
    if len(data) != OTADATA_SIZE:
        raise OtadataError(
            f"{label} must be exactly 0x{OTADATA_SIZE:x} bytes, got 0x{len(data):x}"
        )
    for offset, value in enumerate(data):
        if value != ERASED_BYTE:
            raise OtadataError(
                f"{label} must be fully erased 0xff; byte 0x{offset:x} is 0x{value:02x}"
            )


def validate_path(path: Path) -> None:
    if path.is_symlink() or not path.is_file():
        raise OtadataError(f"otadata input is missing, non-regular or symlinked: {path}")
    try:
        mode = path.stat().st_mode
        if not stat.S_ISREG(mode):
            raise OtadataError(f"otadata input is not a regular file: {path}")
        data = path.read_bytes()
    except OSError as exc:
        raise OtadataError(f"cannot read otadata input {path}: {exc}") from exc
    validate_bytes(data, str(path))


def expect_rejected(data: bytes, label: str) -> None:
    try:
        validate_bytes(data, label)
    except OtadataError:
        return
    raise OtadataError(f"self-test accepted invalid fixture: {label}")


def self_test() -> None:
    canonical = bytes([ERASED_BYTE]) * OTADATA_SIZE
    validate_bytes(canonical, "canonical")

    one_byte = bytearray(canonical)
    one_byte[0x123] = 0xFE
    expect_rejected(bytes(one_byte), "one-byte mutation")
    expect_rejected(bytes(OTADATA_SIZE), "zero-filled mutation")
    expect_rejected(canonical[:-1], "short-size mutation")
    expect_rejected(canonical + bytes([ERASED_BYTE]), "long-size mutation")

    with tempfile.TemporaryDirectory(prefix="otadata-contract-") as directory:
        root = Path(directory)
        regular = root / "ota_data_initial.bin"
        regular.write_bytes(canonical)
        validate_path(regular)
        link = root / "ota-link.bin"
        try:
            link.symlink_to(regular)
        except (OSError, NotImplementedError):
            pass
        else:
            try:
                validate_path(link)
            except OtadataError:
                pass
            else:
                raise OtadataError("self-test accepted a symlinked otadata input")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.path is None and not args.self_test:
        parser.error("path is required unless --self-test is used")
    try:
        if args.path is not None:
            validate_path(args.path)
        if args.self_test:
            self_test()
    except (OSError, OtadataError) as exc:
        print(f"otadata-contract: {exc}", file=sys.stderr)
        return 1
    print("otadata-contract: PASS" + (" (mutation canaries)" if args.self_test else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
