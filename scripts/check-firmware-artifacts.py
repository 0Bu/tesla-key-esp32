#!/usr/bin/env python3
"""Key-free structural and identity validation for ESP-IDF firmware artifacts."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path


IMAGE_MAGIC = 0xE9
APP_DESC_MAGIC = 0xABCD5432
CHECKSUM_SEED = 0xEF
SECURE_BOOT_V2_BLOCK_MAGIC = 0xE7
SIGNATURE_ALIGNMENT = 0x10000
SIGNATURE_SECTOR = 0x1000
SIGNATURE_BLOCK_SIZE = 1216
SIGNATURE_DATA_SIZE = 1196
RSA_BYTES = 384
RSA_N_OFFSET = 36
RSA_E_OFFSET = RSA_N_OFFSET + RSA_BYTES
RSA_RINV_OFFSET = RSA_E_OFFSET + 4
RSA_MPRIME_OFFSET = RSA_RINV_OFFSET + RSA_BYTES
RSA_SIGNATURE_OFFSET = RSA_MPRIME_OFFSET + 4
RSA_PUBLIC_FIELDS_END = RSA_SIGNATURE_OFFSET
MAX_SEGMENTS = 16
MAX_SEGMENT_BYTES = 16 * 1024 * 1024
APP_POLICY_LIMIT = 0x1E8000

CHIP_IDS = {
    "esp32": 0x0000,
    "esp32s3": 0x0009,
    "esp32c3": 0x0005,
    "esp32c6": 0x000D,
}


class ArtifactError(ValueError):
    pass


@dataclass(frozen=True)
class ImageInfo:
    chip_id: int
    segment_count: int
    core_size: int
    version: str | None
    project: str | None
    public_key_digest: str | None = None


# Deterministic, disposable RSA-3072 key used only to build cryptographically valid self-test
# fixtures. It is deliberately not PEM material and has no relationship to the production key.
TEST_RSA_N = int(
    "9a6d1d83b465753ef0c8d1e48668e42f7dfa2d5d5b07319c86f9593d93aa3b07082fab79"
    "75b639938543942564df73abcf4bf371e0eedb019fbc85d7b3fb53c737b1a85f84a4eb9f0"
    "94745fbd8f698532e68e2f2b829bbbe6393c83447eb435f4780d08d55587b9d5c27d3e5a"
    "9f10da3e21a32d88bc7f2319a78fe4066290f6fc7de9ec4140094bebdedaf5623e29be2dd"
    "961ad44dda0001fc82e5d8c5b20d5337cee200219fca79a53b76cb071f765df25166333ea8"
    "c7197820b30ce58f396597663851a551853facc1f009136e5fc008c09c43af6a48ee8c2ee"
    "7767dbb799a481a6d7b35c7ff9663a765391e5b55278bbfd20423904c2a8da6749557cdbb"
    "4a6162d9a96fbd525325393cd40865b1fca3dfda9574c43184963b4c89d9d669ed3f21124"
    "ea79d12405cac5d23b75b9cae251bae709272acc4d440d43bae63a67afcdda64dd9006bd25"
    "6dee8e99a464af263e743c1675c0ca0e039b519ee2ae71b626b2a7d408a34c5dcd676333d"
    "15ea02da42a4ea1d51e7a634f4a2b44222d5db",
    16,
)
TEST_RSA_D = int(
    "18daf282d8a347e16f3748c9391fb83288aff40df0467d240e2423754cc77bceb59b99443d"
    "bfac640e7157c3ee5e24d65a26db07d0f0c0f69c48e75b563b0a90a869d8dedee65337a5"
    "7254ffa491343d41dd5c973d306d13c7753ed10e0e537eb4c48aa20f98394a94f84d7121"
    "6437677c4f43db24cc7432ab03f47055c1201c996a9485476ee72cb83c81c477417f0909d"
    "4a226da624f2ead54e83d21677776407cf4ffee6988ae56fba0dd8172529bebbcfc451d3db"
    "9d0b8a16810430145772620de766f691f4c06a7825137bb785133402533efb390c00002888"
    "4f6a138ea0ea4774ff016cd082498c0011eb3366090ca0c9ebc46e3958183c718965a41166"
    "ad0437677be1b0b7a210499001d83b42f89a6f4ff0c2bf906cc85914a7b643b5d91cc8e25"
    "c96ec8253626f73f74a522818bb2705a3a570fdf87edca6206d635975d98a8f49bfb649844"
    "c4fe7d53afb284cd43bc55271bbe4dc8aa1af469c9c7b967c30bd0971c80299527f0fddf0"
    "095d79ed9b61f3891e081001c48adf75031",
    16,
)
TEST_RSA_E = 65537


def regular_bytes(path: Path, label: str) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ArtifactError(f"{label} is missing, non-regular or a symlink: {path}")
    data = path.read_bytes()
    if not data:
        raise ArtifactError(f"{label} is empty: {path}")
    return data


def c_string(raw: bytes, label: str) -> str:
    if b"\x00" not in raw:
        raise ArtifactError(f"{label} is not NUL terminated")
    value = raw.split(b"\x00", 1)[0]
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ArtifactError(f"{label} is not UTF-8") from exc


def mgf1_sha256(seed: bytes, length: int) -> bytes:
    output = bytearray()
    counter = 0
    while len(output) < length:
        output.extend(hashlib.sha256(seed + counter.to_bytes(4, "big")).digest())
        counter += 1
    return bytes(output[:length])


def verify_rsa_pss_sha256(n: int, e: int, signature: bytes, digest: bytes) -> None:
    """Verify ESP Secure Boot v2's RSA-PSS/SHA-256 signature using only stdlib."""
    if len(signature) != RSA_BYTES or len(digest) != hashlib.sha256().digest_size:
        raise ArtifactError("invalid RSA-PSS signature or digest length")
    if n.bit_length() != RSA_BYTES * 8 or e < 3 or e % 2 == 0:
        raise ArtifactError("invalid RSA-3072 public key")
    encoded = pow(int.from_bytes(signature, "big"), e, n).to_bytes(RSA_BYTES, "big")
    hash_size = hashlib.sha256().digest_size
    db_size = RSA_BYTES - hash_size - 1
    if encoded[-1] != 0xBC or encoded[0] & 0x80:
        raise ArtifactError("Secure Boot v2 RSA-PSS encoding is invalid")
    masked_db = encoded[:db_size]
    encoded_hash = encoded[db_size : db_size + hash_size]
    mask = mgf1_sha256(encoded_hash, db_size)
    database = bytearray(left ^ right for left, right in zip(masked_db, mask))
    database[0] &= 0x7F  # emBits = modulusBits - 1 for RSA-PSS
    salt_size = 32
    separator = db_size - salt_size - 1
    if any(database[:separator]) or database[separator] != 0x01:
        raise ArtifactError("Secure Boot v2 RSA-PSS padding is invalid")
    salt = bytes(database[-salt_size:])
    expected_hash = hashlib.sha256(b"\x00" * 8 + digest + salt).digest()
    if not hmac.compare_digest(encoded_hash, expected_hash):
        raise ArtifactError("Secure Boot v2 RSA-PSS signature verification failed")


def read_public_key_digest(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise ArtifactError(f"public-key digest pin is missing or symlinked: {path}")
    try:
        text = path.read_text(encoding="ascii")
    except UnicodeError as exc:
        raise ArtifactError(f"public-key digest pin is not ASCII: {path}") from exc
    if len(text) != 65 or not text.endswith("\n"):
        raise ArtifactError("public-key digest pin must be exactly 64 lowercase hex characters")
    value = text[:-1]
    if any(char not in "0123456789abcdef" for char in value):
        raise ArtifactError("public-key digest pin must be exactly 64 lowercase hex characters")
    return bytes.fromhex(value)


def parse_image(
    data: bytes,
    *,
    signed: bool,
    expect_app_desc: bool,
    expected_public_key_digest: bytes | None = None,
) -> ImageInfo:
    if len(data) < 24:
        raise ArtifactError("ESP image is shorter than its 24-byte header")
    (
        magic,
        segment_count,
        _spi_mode,
        _spi_speed_size,
        _entry_addr,
        _wp_pin,
        _drv0,
        _drv1,
        _drv2,
        chip_id,
        _min_chip_rev,
        _min_chip_rev_full,
        _max_chip_rev_full,
        _reserved,
        hash_appended,
    ) = struct.unpack_from("<BBBBIBBBBHBHH4sB", data, 0)
    if magic != IMAGE_MAGIC:
        raise ArtifactError(f"invalid ESP image magic: {magic:#x}")
    if not 1 <= segment_count <= MAX_SEGMENTS:
        raise ArtifactError(f"invalid ESP segment count: {segment_count}")
    if hash_appended not in (0, 1):
        raise ArtifactError(f"invalid ESP hash-appended flag: {hash_appended}")

    cursor = 24
    checksum = CHECKSUM_SEED
    first_segment: bytes | None = None
    for index in range(segment_count):
        if cursor + 8 > len(data):
            raise ArtifactError(f"segment {index} header is truncated")
        _load_address, length = struct.unpack_from("<II", data, cursor)
        cursor += 8
        if length > MAX_SEGMENT_BYTES or cursor + length > len(data):
            raise ArtifactError(f"segment {index} has an invalid/truncated length: {length}")
        segment = data[cursor : cursor + length]
        if first_segment is None:
            first_segment = segment
        for byte in segment:
            checksum ^= byte
        cursor += length

    checksum_offset = cursor + (15 - cursor % 16)
    if checksum_offset >= len(data):
        raise ArtifactError("ESP image checksum byte is missing")
    padding = data[cursor:checksum_offset]
    if any(padding):
        raise ArtifactError("ESP image checksum padding is not zero-filled")
    if data[checksum_offset] != checksum:
        raise ArtifactError("ESP image segment checksum mismatch")
    core_size = checksum_offset + 1
    if hash_appended:
        digest_end = core_size + hashlib.sha256().digest_size
        if digest_end > len(data):
            raise ArtifactError("ESP image appended SHA-256 is truncated")
        expected_digest = hashlib.sha256(data[:core_size]).digest()
        if data[core_size:digest_end] != expected_digest:
            raise ArtifactError("ESP image appended SHA-256 mismatch")
        core_size = digest_end

    public_key_digest: str | None = None
    if signed:
        if len(data) > APP_POLICY_LIMIT:
            raise ArtifactError(
                f"signed app is {len(data)} bytes, over policy limit {APP_POLICY_LIMIT}"
            )
        if len(data) < core_size + SIGNATURE_SECTOR:
            raise ArtifactError("signed app has no Secure Boot v2 signature sector")
        signature_offset = len(data) - SIGNATURE_SECTOR
        expected_signature_offset = (
            (core_size + SIGNATURE_ALIGNMENT - 1) // SIGNATURE_ALIGNMENT
        ) * SIGNATURE_ALIGNMENT
        if signature_offset != expected_signature_offset:
            raise ArtifactError(
                "signed app signature sector is not at the minimal 64 KiB-aligned offset: "
                f"expected {expected_signature_offset}, got {signature_offset}"
            )
        if len(data) != expected_signature_offset + SIGNATURE_SECTOR:
            raise ArtifactError("signed app size does not exactly match its signature projection")
        if any(byte != 0xFF for byte in data[core_size:signature_offset]):
            raise ArtifactError("signed app alignment padding is not erased-flash bytes")
        sector = data[signature_offset:]
        block = sector[:SIGNATURE_BLOCK_SIZE]
        if block[0] != SECURE_BOOT_V2_BLOCK_MAGIC or block[1] != 0x02:
            raise ArtifactError("signed app has no Secure Boot v2 signature-block magic")
        stored_crc = struct.unpack_from("<I", block, SIGNATURE_DATA_SIZE)[0]
        calculated_crc = zlib.crc32(block[:SIGNATURE_DATA_SIZE]) & 0xFFFFFFFF
        if stored_crc != calculated_crc:
            raise ArtifactError("signed app Secure Boot v2 signature-block CRC mismatch")
        if any(block[SIGNATURE_DATA_SIZE + 4 :]):
            raise ArtifactError("signed app Secure Boot v2 signature-block padding is not zero")
        if any(byte != 0xFF for byte in sector[SIGNATURE_BLOCK_SIZE:]):
            raise ArtifactError("signed app unused Secure Boot v2 signature blocks are not erased")
        if block[4:36] != hashlib.sha256(data[:signature_offset]).digest():
            raise ArtifactError("signed app Secure Boot v2 image digest mismatch")
        modulus = int.from_bytes(block[RSA_N_OFFSET:RSA_E_OFFSET], "little")
        exponent = struct.unpack_from("<I", block, RSA_E_OFFSET)[0]
        if modulus.bit_length() != 3072 or modulus % 2 == 0 or exponent < 3 or exponent % 2 == 0:
            raise ArtifactError("signed app Secure Boot v2 block is not structurally RSA-3072")
        key_digest_bytes = hashlib.sha256(block[RSA_N_OFFSET:RSA_PUBLIC_FIELDS_END]).digest()
        public_key_digest = key_digest_bytes.hex()
        if (
            expected_public_key_digest is not None
            and not hmac.compare_digest(key_digest_bytes, expected_public_key_digest)
        ):
            raise ArtifactError(
                "signed app public-key digest differs from the pinned production authority"
            )
        verify_rsa_pss_sha256(
            modulus,
            exponent,
            block[RSA_SIGNATURE_OFFSET:SIGNATURE_DATA_SIZE][::-1],
            block[4:36],
        )
    elif len(data) != core_size:
        raise ArtifactError("unsigned ESP image has unexpected trailing bytes")
    elif expected_public_key_digest is not None:
        raise ArtifactError("a public-key digest pin is only valid for signed applications")

    version: str | None = None
    project: str | None = None
    if expect_app_desc:
        if first_segment is None or len(first_segment) < 80:
            raise ArtifactError("application descriptor is missing from the first segment")
        if struct.unpack_from("<I", first_segment, 0)[0] != APP_DESC_MAGIC:
            raise ArtifactError("application descriptor magic is missing from the first segment")
        version = c_string(first_segment[16:48], "application version")
        project = c_string(first_segment[48:80], "application project name")
    return ImageInfo(chip_id, segment_count, core_size, version, project, public_key_digest)


def validate_app(
    target: str,
    version: str,
    app: Path,
    *,
    signed_app: bool,
    expected_public_key_digest: bytes | None = None,
) -> ImageInfo:
    if target not in CHIP_IDS:
        raise ArtifactError(f"unsupported target: {target}")
    if not version or len(version.encode("utf-8")) > 31:
        raise ArtifactError("expected application version must fit the 31-byte IDF descriptor")
    app_info = parse_image(
        regular_bytes(app, f"{target} application"),
        signed=signed_app,
        expect_app_desc=True,
        expected_public_key_digest=expected_public_key_digest,
    )
    expected_chip = CHIP_IDS[target]
    if app_info.chip_id != expected_chip:
        raise ArtifactError(
            f"{target} app advertises chip ID {app_info.chip_id:#x}, expected {expected_chip:#x}"
        )
    if app_info.version != version:
        raise ArtifactError(
            f"{target} app version mismatch: expected {version!r}, got {app_info.version!r}"
        )
    if app_info.project != "tesla-key-esp32":
        raise ArtifactError(
            f"{target} app project mismatch: expected 'tesla-key-esp32', got {app_info.project!r}"
        )
    return app_info


def validate_set(
    target: str,
    version: str,
    bootloader: Path,
    app: Path,
    *,
    signed_app: bool,
    expected_public_key_digest: bytes | None = None,
) -> tuple[ImageInfo, ImageInfo]:
    boot_info = parse_image(
        regular_bytes(bootloader, f"{target} bootloader"),
        signed=False,
        expect_app_desc=False,
    )
    app_info = validate_app(
        target,
        version,
        app,
        signed_app=signed_app,
        expected_public_key_digest=expected_public_key_digest,
    )
    expected_chip = CHIP_IDS[target]
    if boot_info.chip_id != expected_chip:
        raise ArtifactError(
            f"{target} bootloader advertises chip ID {boot_info.chip_id:#x}, expected {expected_chip:#x}"
        )
    return boot_info, app_info


def make_image(target: str, version: str, *, app: bool) -> bytes:
    segment = bytearray(256)
    if app:
        struct.pack_into("<I", segment, 0, APP_DESC_MAGIC)
        segment[16 : 16 + len(version)] = version.encode("utf-8")
        project = b"tesla-key-esp32"
        segment[48 : 48 + len(project)] = project
    else:
        segment[:4] = b"BOOT"
    header = struct.pack(
        "<BBBBIBBBBHBHH4sB",
        IMAGE_MAGIC,
        1,
        0,
        0,
        0x40000000,
        0xEE,
        0,
        0,
        0,
        CHIP_IDS[target],
        0,
        0,
        0,
        b"\x00" * 4,
        1,
    )
    image = bytearray(header + struct.pack("<II", 0x3F400020, len(segment)) + segment)
    checksum = CHECKSUM_SEED
    for byte in segment:
        checksum ^= byte
    image.extend(b"\x00" * (15 - len(image) % 16))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return bytes(image)


def emsa_pss_encode_sha256(digest: bytes, salt: bytes) -> bytes:
    if len(digest) != 32 or len(salt) != 32:
        raise ValueError("RSA-PSS self-test digest/salt must be 32 bytes")
    encoded_hash = hashlib.sha256(b"\x00" * 8 + digest + salt).digest()
    db_size = RSA_BYTES - len(encoded_hash) - 1
    database = b"\x00" * (db_size - len(salt) - 1) + b"\x01" + salt
    mask = mgf1_sha256(encoded_hash, db_size)
    masked_db = bytearray(left ^ right for left, right in zip(database, mask))
    masked_db[0] &= 0x7F
    return bytes(masked_db) + encoded_hash + b"\xbc"


def fake_sign(image: bytes, *, extra_alignment_blocks: int = 0) -> bytes:
    if extra_alignment_blocks < 0:
        raise ValueError("extra alignment block count must be non-negative")
    padding = (-len(image)) % SIGNATURE_ALIGNMENT
    aligned = image + b"\xff" * (padding + extra_alignment_blocks * SIGNATURE_ALIGNMENT)
    block = bytearray(SIGNATURE_DATA_SIZE)
    block[0] = SECURE_BOOT_V2_BLOCK_MAGIC
    block[1] = 0x02
    digest = hashlib.sha256(aligned).digest()
    block[4:36] = digest
    block[RSA_N_OFFSET:RSA_E_OFFSET] = TEST_RSA_N.to_bytes(RSA_BYTES, "little")
    struct.pack_into("<I", block, RSA_E_OFFSET, TEST_RSA_E)
    rinv = (1 << (RSA_BYTES * 8 * 2)) % TEST_RSA_N
    block[RSA_RINV_OFFSET:RSA_MPRIME_OFFSET] = rinv.to_bytes(RSA_BYTES, "little")
    mprime = (-pow(TEST_RSA_N, -1, 1 << 32)) & 0xFFFFFFFF
    struct.pack_into("<I", block, RSA_MPRIME_OFFSET, mprime)
    salt = hashlib.sha256(b"tesla-key-esp32 self-test RSA-PSS salt" + digest).digest()
    encoded = emsa_pss_encode_sha256(digest, salt)
    signature = pow(int.from_bytes(encoded, "big"), TEST_RSA_D, TEST_RSA_N)
    block[RSA_SIGNATURE_OFFSET:SIGNATURE_DATA_SIZE] = signature.to_bytes(
        RSA_BYTES, "big"
    )[::-1]
    block.extend(struct.pack("<I", zlib.crc32(block) & 0xFFFFFFFF))
    block.extend(b"\x00" * 16)
    return aligned + bytes(block) + b"\xff" * (SIGNATURE_SECTOR - len(block))


def self_test() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        boot = root / "bootloader.bin"
        app = root / "app.bin"
        boot.write_bytes(make_image("esp32c6", "ignored", app=False))
        app.write_bytes(make_image("esp32c6", "1.2.3-test", app=True))
        validate_set("esp32c6", "1.2.3-test", boot, app, signed_app=False)

        signed = root / "app-signed.bin"
        signed.write_bytes(fake_sign(app.read_bytes()))
        validate_set("esp32c6", "1.2.3-test", boot, signed, signed_app=True)

        test_pin = hashlib.sha256(
            signed.read_bytes()[
                -SIGNATURE_SECTOR + RSA_N_OFFSET : -SIGNATURE_SECTOR + RSA_PUBLIC_FIELDS_END
            ]
        ).digest()
        validate_set(
            "esp32c6",
            "1.2.3-test",
            boot,
            signed,
            signed_app=True,
            expected_public_key_digest=test_pin,
        )
        app_info = validate_app(
            "esp32c6",
            "1.2.3-test",
            signed,
            signed_app=True,
            expected_public_key_digest=test_pin,
        )
        assert app_info.public_key_digest == test_pin.hex()
        try:
            validate_set(
                "esp32c6",
                "1.2.3-test",
                boot,
                signed,
                signed_app=True,
                expected_public_key_digest=b"\x00" * 32,
            )
        except ArtifactError as exc:
            assert "pinned production authority" in str(exc)
        else:
            raise AssertionError("wrong Secure Boot v2 public-key pin was accepted")

        # A structurally self-consistent signature one whole alignment block later wastes OTA
        # capacity and makes the pre-sign projection dishonest. Only the exact minimal offset is
        # accepted, not merely an arbitrary 64 KiB-aligned one.
        signed.write_bytes(fake_sign(app.read_bytes(), extra_alignment_blocks=1))
        try:
            validate_set("esp32c6", "1.2.3-test", boot, signed, signed_app=True)
        except ArtifactError as exc:
            assert "minimal 64 KiB-aligned offset" in str(exc)
        else:
            raise AssertionError("extra 64 KiB Secure Boot alignment block was accepted")

        signed.write_bytes(fake_sign(app.read_bytes()))

        # Software-only TOFU in ESP-IDF v5.5 trusts only position zero. A second block must not be
        # accepted as a pretend OTA key-rotation bridge; that transition is explicitly USB-only.
        extra_signature = bytearray(signed.read_bytes())
        extra_signature[-SIGNATURE_SECTOR + SIGNATURE_BLOCK_SIZE] = SECURE_BOOT_V2_BLOCK_MAGIC
        signed.write_bytes(extra_signature)
        try:
            validate_set("esp32c6", "1.2.3-test", boot, signed, signed_app=True)
        except ArtifactError as exc:
            assert "unused Secure Boot v2 signature blocks are not erased" in str(exc)
        else:
            raise AssertionError("second Secure Boot v2 signature block was accepted")

        signed.write_bytes(fake_sign(app.read_bytes()))

        corrupt_signature = bytearray(signed.read_bytes())
        corrupt_signature[-SIGNATURE_SECTOR + RSA_SIGNATURE_OFFSET] ^= 1
        block_start = len(corrupt_signature) - SIGNATURE_SECTOR
        crc = zlib.crc32(
            corrupt_signature[block_start : block_start + SIGNATURE_DATA_SIZE]
        ) & 0xFFFFFFFF
        struct.pack_into(
            "<I", corrupt_signature, block_start + SIGNATURE_DATA_SIZE, crc
        )
        signed.write_bytes(corrupt_signature)
        try:
            validate_set("esp32c6", "1.2.3-test", boot, signed, signed_app=True)
        except ArtifactError as exc:
            assert "RSA-PSS" in str(exc)
        else:
            raise AssertionError("CRC-consistent invalid RSA-PSS signature was accepted")

        try:
            validate_set("esp32c3", "1.2.3-test", boot, app, signed_app=False)
        except ArtifactError:
            pass
        else:
            raise AssertionError("cross-target artifact swap was accepted")

        try:
            validate_set("esp32c6", "1.2.4", boot, app, signed_app=False)
        except ArtifactError:
            pass
        else:
            raise AssertionError("stale application version was accepted")

        corrupted = bytearray(app.read_bytes())
        corrupted[64] ^= 1
        app.write_bytes(corrupted)
        try:
            validate_set("esp32c6", "1.2.3-test", boot, app, signed_app=False)
        except ArtifactError:
            pass
        else:
            raise AssertionError("corrupted ESP image was accepted")
    print("firmware artifact identity self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=tuple(CHIP_IDS))
    parser.add_argument("--version")
    parser.add_argument("--bootloader", type=Path)
    parser.add_argument("--app", type=Path)
    parser.add_argument("--signed-app", action="store_true")
    parser.add_argument("--app-only", action="store_true")
    parser.add_argument("--expected-public-key-digest", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if None in (args.target, args.version, args.app):
        parser.error("--target, --version and --app are required")
    if args.app_only:
        if args.bootloader is not None:
            parser.error("--app-only does not accept --bootloader")
        if not args.signed_app or args.expected_public_key_digest is None:
            parser.error(
                "--app-only requires --signed-app and --expected-public-key-digest"
            )
    elif args.bootloader is None:
        parser.error("--bootloader is required unless --app-only is selected")
    try:
        expected_public_key_digest = (
            read_public_key_digest(args.expected_public_key_digest)
            if args.expected_public_key_digest is not None
            else None
        )
        if args.app_only:
            boot_info = None
            app_info = validate_app(
                args.target,
                args.version,
                args.app,
                signed_app=True,
                expected_public_key_digest=expected_public_key_digest,
            )
        else:
            boot_info, app_info = validate_set(
                args.target,
                args.version,
                args.bootloader,
                args.app,
                signed_app=args.signed_app,
                expected_public_key_digest=expected_public_key_digest,
            )
    except (OSError, UnicodeError, ArtifactError) as exc:
        print(f"firmware artifact validation failed: {exc}", file=sys.stderr)
        return 1
    state = "signed" if args.signed_app else "unsigned"
    key_note = (
        f", public-key-sha256={app_info.public_key_digest}"
        if app_info.public_key_digest is not None
        else ""
    )
    boot_note = (
        f"boot-segments={boot_info.segment_count}, " if boot_info is not None else "app-only, "
    )
    print(
        f"firmware artifact identity: PASS ({args.target}, {state}, version={app_info.version}, "
        f"{boot_note}app-segments={app_info.segment_count}{key_note})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
