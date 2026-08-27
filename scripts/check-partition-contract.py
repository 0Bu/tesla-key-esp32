#!/usr/bin/env python3
"""Fail closed when the source or generated ESP-IDF partition table drifts."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


ENTRY_MAGIC = 0x50AA
MD5_MAGIC = b"\xeb\xeb"
ENTRY_SIZE = 32
TABLE_SIZE_MAX = 0x1000
FLASH_SIZE = 0x400000

TYPE_NAMES = {"app": 0x00, "data": 0x01}
SUBTYPE_NAMES = {
    (0x01, "ota"): 0x00,
    (0x01, "phy"): 0x01,
    (0x01, "nvs"): 0x02,
    (0x01, "coredump"): 0x03,
    (0x00, "ota_0"): 0x10,
    (0x00, "ota_1"): 0x11,
}


@dataclass(frozen=True)
class Entry:
    name: str
    type: int
    subtype: int
    offset: int
    size: int
    flags: int = 0


EXPECTED = (
    Entry("nvs", 0x01, 0x02, 0x9000, 0x6000),
    Entry("otadata", 0x01, 0x00, 0xF000, 0x2000),
    Entry("phy_init", 0x01, 0x01, 0x11000, 0x1000),
    Entry("coredump", 0x01, 0x03, 0x12000, 0xC000),
    Entry("ota_0", 0x00, 0x10, 0x20000, 0x1F0000),
    Entry("ota_1", 0x00, 0x11, 0x210000, 0x1F0000),
)


class ContractError(ValueError):
    pass


def parse_number(value: str, label: str) -> int:
    try:
        parsed = int(value.strip(), 0)
    except ValueError as exc:
        raise ContractError(f"{label} is not an integer: {value!r}") from exc
    if parsed < 0 or parsed > 0xFFFFFFFF:
        raise ContractError(f"{label} is outside uint32 range: {value!r}")
    return parsed


def parse_csv_text(text: str) -> tuple[Entry, ...]:
    entries: list[Entry] = []
    for line_number, row in enumerate(csv.reader(io.StringIO(text)), 1):
        if not row or not "".join(row).strip() or row[0].lstrip().startswith("#"):
            continue
        if len(row) not in (5, 6):
            raise ContractError(f"CSV line {line_number} must contain five or six fields")
        fields = [field.strip() for field in row]
        name, type_text, subtype_text, offset_text, size_text = fields[:5]
        flags_text = fields[5] if len(fields) == 6 else ""
        if not name or len(name.encode("ascii", "strict")) > 15:
            raise ContractError(f"CSV line {line_number} has an invalid partition name")
        try:
            part_type = (
                TYPE_NAMES[type_text] if type_text in TYPE_NAMES else int(type_text, 0)
            )
        except ValueError as exc:
            raise ContractError(f"CSV line {line_number} has an invalid type: {type_text!r}") from exc
        try:
            subtype = (
                SUBTYPE_NAMES[(part_type, subtype_text)]
                if (part_type, subtype_text) in SUBTYPE_NAMES
                else int(subtype_text, 0)
            )
        except ValueError as exc:
            raise ContractError(
                f"CSV line {line_number} has an invalid subtype: {subtype_text!r}"
            ) from exc
        flags = 0 if not flags_text else parse_number(flags_text, f"CSV line {line_number} flags")
        entries.append(
            Entry(
                name,
                part_type,
                subtype,
                parse_number(offset_text, f"CSV line {line_number} offset"),
                parse_number(size_text, f"CSV line {line_number} size"),
                flags,
            )
        )
    return tuple(entries)


def parse_binary(data: bytes) -> tuple[Entry, ...]:
    if not data or len(data) > TABLE_SIZE_MAX or len(data) % ENTRY_SIZE:
        raise ContractError("binary partition table must be 32-byte aligned and at most 0x1000 bytes")
    entries: list[Entry] = []
    md5_seen = False
    for offset in range(0, len(data), ENTRY_SIZE):
        raw = data[offset : offset + ENTRY_SIZE]
        if raw == b"\xff" * ENTRY_SIZE:
            if any(byte != 0xFF for byte in data[offset:]):
                raise ContractError("non-padding data follows the partition-table terminator")
            break
        if raw[:2] == MD5_MAGIC:
            if md5_seen or entries == []:
                raise ContractError("duplicate or misplaced partition-table MD5 record")
            if raw[2:16] != b"\xff" * 14:
                raise ContractError("partition-table MD5 reserved bytes are not erased-flash bytes")
            expected_md5 = hashlib.md5(data[:offset]).digest()  # noqa: S324 - file format contract
            if raw[16:32] != expected_md5:
                raise ContractError("partition-table MD5 does not match preceding entries")
            md5_seen = True
            continue
        if md5_seen:
            raise ContractError("partition entry follows the MD5 record")
        magic, part_type, subtype, part_offset, size, raw_name, flags = struct.unpack(
            "<HBBII16sI", raw
        )
        if magic != ENTRY_MAGIC:
            raise ContractError(f"invalid partition entry magic at byte {offset:#x}")
        name_bytes = raw_name.split(b"\x00", 1)[0]
        try:
            name = name_bytes.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ContractError("partition label is not ASCII") from exc
        if not name:
            raise ContractError("partition label is empty")
        entries.append(Entry(name, part_type, subtype, part_offset, size, flags))
    if not entries:
        raise ContractError("partition table contains no entries")
    if not md5_seen:
        raise ContractError("generated partition table has no ESP-IDF MD5 record")
    return tuple(entries)


def validate(entries: tuple[Entry, ...], origin: str) -> None:
    if entries != EXPECTED:
        expected_rows = [(e.name, hex(e.offset), hex(e.size)) for e in EXPECTED]
        actual_rows = [(e.name, hex(e.offset), hex(e.size)) for e in entries]
        raise ContractError(
            f"{origin} partition contract drifted: expected={expected_rows!r} actual={actual_rows!r}"
        )
    previous_end = 0x9000
    for entry in entries:
        if entry.size <= 0 or entry.offset < previous_end:
            raise ContractError(f"{origin} partition {entry.name} overlaps its predecessor")
        if entry.offset + entry.size > FLASH_SIZE:
            raise ContractError(f"{origin} partition {entry.name} exceeds 4 MiB flash")
        previous_end = entry.offset + entry.size
    if entries[-1].offset + entries[-1].size != FLASH_SIZE:
        raise ContractError(f"{origin} OTA layout does not end exactly at 4 MiB")


def encode_binary(entries: tuple[Entry, ...], *, with_md5: bool = True) -> bytes:
    encoded = bytearray()
    for entry in entries:
        name = entry.name.encode("ascii") + b"\x00"
        encoded.extend(
            struct.pack(
                "<HBBII16sI",
                ENTRY_MAGIC,
                entry.type,
                entry.subtype,
                entry.offset,
                entry.size,
                name.ljust(16, b"\x00"),
                entry.flags,
            )
        )
    if with_md5:
        encoded.extend(MD5_MAGIC + b"\xff" * 14 + hashlib.md5(encoded).digest())  # noqa: S324
    encoded.extend(b"\xff" * (0xC00 - len(encoded)))
    return bytes(encoded)


def self_test() -> None:
    csv_text = """\
nvs,data,nvs,0x9000,0x6000,
otadata,data,ota,0xf000,0x2000,
phy_init,data,phy,0x11000,0x1000,
coredump,data,coredump,0x12000,0xc000,
ota_0,app,ota_0,0x20000,0x1f0000,
ota_1,app,ota_1,0x210000,0x1f0000,
"""
    parsed_csv = parse_csv_text(csv_text)
    validate(parsed_csv, "fixture CSV")
    binary = encode_binary(parsed_csv)
    validate(parse_binary(binary), "fixture binary")

    bad_csv = csv_text.replace("0x210000,0x1f0000", "0x211000,0x1ef000")
    try:
        validate(parse_csv_text(bad_csv), "mutated CSV")
    except ContractError:
        pass
    else:
        raise AssertionError("offset/size mutation was accepted")

    tampered = bytearray(binary)
    tampered[ENTRY_SIZE + 12] ^= 1
    try:
        parse_binary(bytes(tampered))
    except ContractError:
        pass
    else:
        raise AssertionError("binary/MD5 mutation was accepted")

    try:
        parse_binary(encode_binary(parsed_csv, with_md5=False))
    except ContractError:
        pass
    else:
        raise AssertionError("binary partition table without an MD5 record was accepted")

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "partitions.csv"
        path.write_text(csv_text, encoding="utf-8")
        validate(parse_csv_text(path.read_text(encoding="utf-8")), str(path))
    print("partition source/binary contract self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.csv is None:
        parser.error("--csv is required unless --self-test is used")
    try:
        csv_entries = parse_csv_text(args.csv.read_text(encoding="utf-8"))
        validate(csv_entries, str(args.csv))
        if args.binary is not None:
            binary_entries = parse_binary(args.binary.read_bytes())
            validate(binary_entries, str(args.binary))
            if binary_entries != csv_entries:
                raise ContractError("generated binary table differs from the validated CSV")
    except (OSError, UnicodeError, ContractError) as exc:
        print(f"partition contract failed: {exc}", file=sys.stderr)
        return 1
    suffix = " + generated binary" if args.binary is not None else ""
    print(f"partition contract: PASS (source CSV{suffix}, {len(csv_entries)} exact entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
