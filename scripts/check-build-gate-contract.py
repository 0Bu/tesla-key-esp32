#!/usr/bin/env python3
"""Static, mutation-tested contract for the four-target build-to-release gate chain."""

from __future__ import annotations

import argparse
import ast
from collections import Counter
from pathlib import Path
import re
import shutil
import sys
import tempfile
from typing import Any


TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
CANONICAL_DISPLAY_VERSION_PATTERN = (
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\."
    r"(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$"
)
SHELL_VERSION_ASSIGNMENT = f"VERSION_RE='{CANONICAL_DISPLAY_VERSION_PATTERN}'"
COMPILER_INJECTION_ENV = (
    "CPATH",
    "CPLUS_INCLUDE_PATH",
    "C_INCLUDE_PATH",
    "OBJC_INCLUDE_PATH",
    "DEPENDENCIES_OUTPUT",
    "SUNPRO_DEPENDENCIES",
    "GCC_EXEC_PREFIX",
    "COMPILER_PATH",
    "LIBRARY_PATH",
)
UNSIGNED_TARGET_FILES = (
    "bootloader/bootloader.bin",
    "partition_table/partition-table.bin",
    "tesla-key-esp32.bin",
    "ota_data_initial.bin",
)
DIST_TARGET_FILES = (
    "tesla-key-esp32-{target}.elf",
    "tesla-key-esp32-{target}.elf.sha256",
    "tesla-key-esp32-{target}.map",
    "sdkconfig.{target}",
    "dependencies.lock.{target}",
    "size-{target}.json",
    "size-{target}.md",
    "projected-signed-size.txt",
    "stack-usage-{target}.json",
)
DIST_GLOBAL_FILES = ("build-metadata.txt",)
SOURCE_EXCLUSIONS = ("version.txt",)
EXPECTED_PAYLOAD_COUNT = 53
CHIP_IDS = {"esp32": 0x0000, "esp32s3": 0x0009, "esp32c3": 0x0005, "esp32c6": 0x000D}
MANIFEST_TARGETS = (
    ("ESP32", "esp32", "", 0x1000),
    ("ESP32-S3", "esp32s3", "-s3", 0),
    ("ESP32-C3", "esp32c3", "-c3", 0),
    ("ESP32-C6", "esp32c6", "-c6", 0),
)
MERGED_NAMES = {
    "ESP32": "tesla-key-esp32-{version}-merged.bin",
    "ESP32-S3": "tesla-key-esp32-s3-{version}-merged.bin",
    "ESP32-C3": "tesla-key-esp32-c3-{version}-merged.bin",
    "ESP32-C6": "tesla-key-esp32-c6-{version}-merged.bin",
}
EXPECTED_OFFSETS = {
    "ESP32": (0x1000, 0x8000, 0x20000, 0xF000),
    "ESP32-S3": (0, 0x8000, 0x20000, 0xF000),
    "ESP32-C3": (0, 0x8000, 0x20000, 0xF000),
    "ESP32-C6": (0, 0x8000, 0x20000, 0xF000),
}
EXACT_TARGET_LOOP = "for target in esp32 esp32s3 esp32c3 esp32c6; do"
HEAD_REF = "ref: ${{ github.event.pull_request.head.sha || github.sha }}"
SOURCE_EXPR = '"${{ github.event.pull_request.head.sha || github.sha }}"'
LOCAL_PAGES_CHECK = "python3 scripts/check-release-pages-bytes.py"
PAGES_SOURCE_CHECK = "python3 scripts/check-pages-source.py"
TRUSTED_DEFAULT_ENV = "TRUSTED_DEFAULT_SHA: ${{ github.sha }}"
TRUSTED_DEFAULT_FETCH = "git fetch --no-tags origin"
TRUSTED_DEFAULT_COMPARE = 'if [ "$current_default" != "$TRUSTED_DEFAULT_SHA" ]; then'
FINAL_RELEASE_API = 'gh api "repos/$GITHUB_REPOSITORY/releases/tags/v$RELEASE_VERSION" > "$release_json"'
FINAL_RELEASE_CHECK = "python3 scripts/check-published-release.py"
FINAL_RELEASE_JSON_ARG = '--release-json "$release_json"'
DRAFT_RELEASE_API = 'gh api "repos/$GITHUB_REPOSITORY/releases/$DRAFT_RELEASE_ID" > "$draft_json"'
DRAFT_RELEASE_CHECK = "--expect-state draft"
PUBLISH_DRAFT = 'gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$release_id"'
IMMUTABLE_RELEASE_CHECK = "--expect-state published-immutable"
SIGNED_ROOT_TEMPLATES = (
    "tesla-key-esp32.bin",
    "tesla-key-esp32-s3.bin",
    "tesla-key-esp32-c3.bin",
    "tesla-key-esp32-c6.bin",
    "tesla-key-esp32-{version}.bin",
    "tesla-key-esp32-s3-{version}.bin",
    "tesla-key-esp32-c3-{version}.bin",
    "tesla-key-esp32-c6-{version}.bin",
    "tesla-key-esp32-{version}-merged.bin",
    "tesla-key-esp32-s3-{version}-merged.bin",
    "tesla-key-esp32-c3-{version}-merged.bin",
    "tesla-key-esp32-c6-{version}-merged.bin",
)
SIGNED_STAGE_PATHS = tuple(
    f"_fw/{target}/{name}"
    for target in TARGETS
    for name in (
        "bootloader.bin", "partition-table.bin", "tesla-key-esp32.bin",
        "ota_data_initial.bin",
    )
)
REQUIRED_FILES = (
    ".gitignore",
    ".github/workflows/build.yml",
    ".github/workflows/signed-pr-preview.yml",
    "scripts/build-pages.sh",
    "scripts/check-dependency-contract.py",
    "scripts/check-build-artifact-inventory.py",
    "scripts/check-bench-acceptance.py",
    "scripts/check-build-semantics.py",
    "scripts/check-firmware-artifacts.py",
    "scripts/ota-signing-public-key.sha256",
    "scripts/check-pages-manifest.py",
    "scripts/check-pages-source.py",
    "scripts/check-otadata-contract.py",
    "scripts/check-published-release.py",
    "scripts/check-release-assets.py",
    "scripts/check-signed-root-inventory.py",
    "scripts/prepare-reused-release.py",
    "scripts/check-release-pages-bytes.py",
    "scripts/check-reproducible-build.sh",
    "scripts/idf-docker.sh",
    "scripts/ci-build-all.sh",
    "scripts/ci-build-verify.sh",
    "scripts/ci-sign-artifacts.sh",
    "scripts/report-firmware-size.py",
    "scripts/release-relevance.sh",
    "scripts/select-release-version.sh",
    "scripts/test-release-contract.sh",
    "scripts/test-build-contracts.sh",
    "scripts/run-mock-tests.sh",
    "sdkconfig.defaults",
    "esp-idf-toolchain.txt",
    "main/idf_component.yml",
    "dependencies.lock.esp32",
    "dependencies.lock.esp32s3",
    "dependencies.lock.esp32c3",
    "dependencies.lock.esp32c6",
    "patches/tesla-ble/0001-reject-replayed-carserver-responses.patch",
    "patches/tesla-ble/0002-report-key-regeneration-result.patch",
    "patches/tesla-ble/0003-rate-limit-rx-framing-recovery-logs.patch",
    "test/run-cjson-oom-tests.sh",
    "test/run-mqtt-json-publish-tests.sh",
)


class GateError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def read(root: Path, relative: str) -> str:
    path = root / relative
    require(path.is_file() and not path.is_symlink(), f"missing/unsafe gate source: {relative}")
    return path.read_text(encoding="utf-8")


def require_order(text: str, label: str, markers: tuple[str, ...]) -> None:
    positions: list[int] = []
    for marker in markers:
        position = text.find(marker)
        require(position >= 0, f"{label}: required stage is missing: {marker}")
        positions.append(position)
    require(positions == sorted(positions) and len(set(positions)) == len(positions),
            f"{label}: required stage order drifted")


def workflow_signed_root_paths(text: str) -> Counter[str]:
    return Counter(
        re.findall(r"^            (tesla-key-esp32[^\n]*\.bin)\s*$", text, re.MULTILINE)
    )


def workflow_signed_stage_paths(text: str) -> Counter[str]:
    return Counter(re.findall(r"^            (_fw/[^\n]+)\s*$", text, re.MULTILINE))


def shell_targets(text: str, label: str) -> None:
    matches = re.findall(r'^TARGETS="([^"]*)"\s*$', text, re.MULTILINE)
    require(len(matches) == 1, f"{label}: must declare exactly one literal TARGETS assignment")
    require(tuple(matches[0].split()) == TARGETS,
            f"{label}: target set/order must be exactly {' '.join(TARGETS)}")


def marked_target_loop(
    text: str,
    script_label: str,
    loop_label: str,
    header: str,
    required_tokens: tuple[str, ...],
) -> str:
    """Return one review-marked exact-four loop and reject per-target bypass constructs."""
    begin = f"# EXACT_FOUR_TARGETS_BEGIN {loop_label}"
    end = f"# EXACT_FOUR_TARGETS_END {loop_label}"
    require(text.count(begin) == 1 and text.count(end) == 1,
            f"{script_label}: exact-four loop markers drifted for {loop_label}")
    start = text.index(begin) + len(begin)
    finish = text.index(end)
    require(start < finish, f"{script_label}: reversed exact-four loop markers for {loop_label}")
    region = text[start:finish].strip()
    require(region.startswith(header + "\n") and region.endswith("\ndone"),
            f"{script_label}: {loop_label} must be one exact four-target loop")
    require(region.count(header) == 1,
            f"{script_label}: {loop_label} loop header must occur exactly once")
    for token in required_tokens:
        require(region.count(token) == 1,
                f"{script_label}: {loop_label} loop must reach exactly one {token}")

    require(re.search(r"\b(?:continue|break)\b", region) is None,
            f"{script_label}: {loop_label} loop may not skip a target")
    require(re.search(r"\|\|\s*(?:true\b|:)", region) is None,
            f"{script_label}: {loop_label} loop may not ignore a target failure")
    require(re.search(r"\b(?:exit|return)\s+0\b", region) is None,
            f"{script_label}: {loop_label} loop may not exit successfully before all targets")
    target_ref = r"\$(?:target|\{target\})"
    require(re.search(rf"(?m)^\s*if\b[^\n]*{target_ref}", region) is None,
            f"{script_label}: {loop_label} loop may not branch on target")
    require(re.search(rf"(?m)^\s*case\s+[^\n]*{target_ref}", region) is None,
            f"{script_label}: {loop_label} loop may not case-dispatch on target")
    require(
        re.search(rf"(?m)^\s*\[\[[^\n]*{target_ref}[^\n]*\]\]\s*(?:&&|\|\|)", region)
        is None,
        f"{script_label}: {loop_label} loop may not conditionally skip/invoke by target",
    )
    return region


def literal_assignment(root: Path, relative: str, name: str) -> Any:
    text = read(root, relative)
    values: list[Any] = []
    for node in ast.parse(text, filename=relative).body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if isinstance(target, ast.Name) and target.id == name:
            try:
                values.append(ast.literal_eval(node.value))
            except (ValueError, SyntaxError) as exc:
                raise GateError(f"{relative}: {name} must be a static literal") from exc
    require(len(values) == 1, f"{relative}: expected exactly one static {name} assignment")
    return values[0]


def compiled_regex_assignment(root: Path, relative: str, name: str) -> str:
    values: list[str] = []
    for node in ast.parse(read(root, relative), filename=relative).body:
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        value = node.value
        if not isinstance(target, ast.Name) or target.id != name:
            continue
        if (
            isinstance(value, ast.Call)
            and isinstance(value.func, ast.Attribute)
            and isinstance(value.func.value, ast.Name)
            and value.func.value.id == "re"
            and value.func.attr == "compile"
            and len(value.args) == 1
            and not value.keywords
            and isinstance(value.args[0], ast.Constant)
            and isinstance(value.args[0].value, str)
        ):
            values.append(value.args[0].value)
    require(len(values) == 1, f"{relative}: expected exactly one static re.compile {name}")
    return values[0]


def validate(root: Path) -> None:
    for relative in REQUIRED_FILES:
        read(root, relative)

    build_all = read(root, "scripts/ci-build-all.sh")
    idf_docker = read(root, "scripts/idf-docker.sh")
    dependency_contract = read(root, "scripts/check-dependency-contract.py")
    inventory = read(root, "scripts/check-build-artifact-inventory.py")
    build_semantics = read(root, "scripts/check-build-semantics.py")
    signer = read(root, "scripts/ci-sign-artifacts.sh")
    pages = read(root, "scripts/build-pages.sh")
    pages_manifest = read(root, "scripts/check-pages-manifest.py")
    release_pages = read(root, "scripts/check-release-pages-bytes.py")
    otadata_contract = read(root, "scripts/check-otadata-contract.py")
    verify = read(root, "scripts/ci-build-verify.sh")
    release_test = read(root, "scripts/test-release-contract.sh")
    reproducible = read(root, "scripts/check-reproducible-build.sh")
    release_assets = read(root, "scripts/check-release-assets.py")
    signed_root_inventory = read(root, "scripts/check-signed-root-inventory.py")
    firmware_artifacts = read(root, "scripts/check-firmware-artifacts.py")
    reuse_release = read(root, "scripts/prepare-reused-release.py")
    production_key_digest = read(root, "scripts/ota-signing-public-key.sha256")
    sdkconfig_defaults = read(root, "sdkconfig.defaults")
    published_release = read(root, "scripts/check-published-release.py")
    release_relevance = read(root, "scripts/release-relevance.sh")
    release_selector = read(root, "scripts/select-release-version.sh")
    build_contracts = read(root, "scripts/test-build-contracts.sh")
    run_mock = read(root, "scripts/run-mock-tests.sh")
    build_workflow = read(root, ".github/workflows/build.yml")
    preview_workflow = read(root, ".github/workflows/signed-pr-preview.yml")
    cjson_test = read(root, "test/run-cjson-oom-tests.sh")
    mqtt_json_test = read(root, "test/run-mqtt-json-publish-tests.sh")
    gitignore = read(root, ".gitignore")

    for text, label in (
        (build_all, "scripts/ci-build-all.sh"),
        (signer, "scripts/ci-sign-artifacts.sh"),
        (pages, "scripts/build-pages.sh"),
        (release_test, "scripts/test-release-contract.sh"),
    ):
        require(
            text.count(SHELL_VERSION_ASSIGNMENT) == 1,
            f"{label}: canonical display-version grammar drifted",
        )
    for relative, name in (
        ("scripts/check-signed-root-inventory.py", "VERSION_RE"),
        ("scripts/check-pages-manifest.py", "VERSION_RE"),
        ("scripts/check-release-pages-bytes.py", "VERSION_RE"),
        ("scripts/check-release-assets.py", "VERSION_RE"),
        ("scripts/check-published-release.py", "VERSION_RE"),
        ("scripts/check-bench-acceptance.py", "DISPLAY_VERSION"),
    ):
        require(
            compiled_regex_assignment(root, relative, name)
            == CANONICAL_DISPLAY_VERSION_PATTERN,
            f"{relative}: canonical display-version grammar drifted",
        )
    require(
        "non-canonical leading-zero version" in build_all
        and "self-test accepted non-canonical leading-zero version" in signed_root_inventory
        and "self-test accepted non-canonical leading-zero version" in inventory
        and "non-canonical leading-zero release version was accepted" in release_assets
        and "non-canonical leading-zero release version was accepted" in published_release
        and "non-canonical version" in read(root, "scripts/check-bench-acceptance.py")
        and build_contracts.count("non-canonical leading-zero version accepted") == 4
        and "manifest version self-test failed: non-canonical leading-zero version accepted"
        in build_contracts,
        "canonical display-version leading-zero canaries drifted",
    )

    require(
        gitignore.count("/build_boundary_*/") == 1,
        ".gitignore: boundary-build trees must stay outside the source fingerprint",
    )

    require(build_contracts.count(
                'python3 "$repo_root/scripts/check-pages-source.py" --self-test') == 1,
            "test-build-contracts.sh: branch-backed Pages source self-test is not wired")
    require(build_contracts.count(
                'python3 "$repo_root/scripts/check-dependency-contract.py" --self-test') == 1,
            "test-build-contracts.sh: dependency contract self-test is not wired")
    require(build_contracts.count(
                'python3 "$repo_root/scripts/check-otadata-contract.py" --self-test') == 1,
            "test-build-contracts.sh: otadata contract self-test is not wired")
    require(build_contracts.count(
                'python3 "$repo_root/scripts/prepare-reused-release.py" --self-test') == 1,
            "test-build-contracts.sh: immutable Release reuse self-test is not wired")
    require(build_contracts.count(
                'python3 "$repo_root/scripts/check-signed-root-inventory.py" --self-test') == 1,
            "test-build-contracts.sh: signed root inventory self-test is not wired")
    require("python3 scripts/check-dependency-contract.py --self-test" in run_mock and
            "python3 scripts/check-otadata-contract.py --self-test" in run_mock,
            "run-mock-tests.sh: dependency/otadata gates are not directly wired")
    require("python3 scripts/prepare-reused-release.py --self-test" in run_mock,
            "run-mock-tests.sh: immutable Release reuse gate is not directly wired")
    require(
        "TARGET_SUFFIXES = (\"\", \"-s3\", \"-c3\", \"-c6\")"
        in signed_root_inventory
        and "len(names) == 12" in signed_root_inventory
        and "signed root firmware inventory drifted" in signed_root_inventory
        and "self-test accepted mutation" in signed_root_inventory,
        "check-signed-root-inventory.py: exact twelve-file root contract drifted",
    )

    require(literal_assignment(root, "scripts/check-dependency-contract.py", "TARGETS") == TARGETS,
            "check-dependency-contract.py: exact target lock set/order drifted")
    require(literal_assignment(root, "scripts/check-dependency-contract.py", "TESLA_VERSION") == "v5.1.2" and
            literal_assignment(root, "scripts/check-dependency-contract.py", "TESLA_RESOLVED_COMMIT") ==
            "a0e5efa610e7ee93ca04fd36bed72e9aac03f008" and
            literal_assignment(root, "scripts/check-dependency-contract.py", "TESLA_COMPONENT_HASH") ==
            "1ae0006ec68f649d13b58671950971e29186873114168756bcc4fd832fc5826f",
            "check-dependency-contract.py: tesla-ble version/resolution contract drifted")
    require(literal_assignment(root, "scripts/check-otadata-contract.py", "OTADATA_SIZE") == 0x2000 and
            literal_assignment(root, "scripts/check-otadata-contract.py", "ERASED_BYTE") == 0xFF,
            "check-otadata-contract.py: exact 0x2000/all-0xff contract drifted")
    require("one-byte mutation" in otadata_contract and
            "zero-filled mutation" in otadata_contract and
            "short-size mutation" in otadata_contract and
            "long-size mutation" in otadata_contract,
            "check-otadata-contract.py: content/zero/size mutation canaries drifted")
    require(
        production_key_digest
        == "0cfdce59b489a8a1a285a4f5b39b4332061d6f649ae7948d3804279378381889\n",
        "production Secure Boot v2 public-key digest pin drifted",
    )
    require(
        "def verify_rsa_pss_sha256(" in firmware_artifacts
        and "Secure Boot v2 RSA-PSS signature verification failed" in firmware_artifacts
        and "expected_public_key_digest" in firmware_artifacts
        and "wrong Secure Boot v2 public-key pin was accepted" in firmware_artifacts
        and "second Secure Boot v2 signature block was accepted" in firmware_artifacts
        and "CRC-consistent invalid RSA-PSS signature was accepted" in firmware_artifacts
        and "def validate_app(" in firmware_artifacts
        and 'parser.add_argument("--app-only", action="store_true")' in firmware_artifacts
        and "--app-only requires --signed-app and --expected-public-key-digest" in firmware_artifacts
        and "assert app_info.public_key_digest == test_pin.hex()" in firmware_artifacts,
        "check-firmware-artifacts.py: RSA-PSS/public-key authority gate or canaries drifted",
    )
    require(
        "PRODUCTION_KEY_DIGEST_PATH" in reuse_release
        and "expected_public_key_digest=expected_public_key_digest" in reuse_release
        and "self-test accepted a non-authoritative signing key" in reuse_release
        and '"rsa-signature", tamper_signature, "RSA-PSS"' in reuse_release
        and "valid fixture did not stage twelve root recovery artifacts" in reuse_release,
        "prepare-reused-release.py: production-key/recovery staging contract drifted",
    )
    require(
        "def read_snapshot_asset(directory_fd: int, name: str) -> bytes:" in reuse_release
        and "os.O_RDONLY | os.O_NOFOLLOW" in reuse_release
        and "snapshot = snapshot_flat_download(download, version)" in reuse_release
        and "validate_remote_bytes(release, snapshot, version, source_sha)" in reuse_release,
        "prepare-reused-release.py: no-follow immutable Release snapshot contract drifted",
    )
    require(
        "after_snapshot_validation=replace_download_paths_after_metadata" in reuse_release
        and "post-metadata path swap changed staged merged snapshot bytes" in reuse_release,
        "prepare-reused-release.py: post-metadata path-swap snapshot canary drifted",
    )
    require(
        sdkconfig_defaults.count("CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT=n") == 1
        and sdkconfig_defaults.count("CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y") == 1,
        "sdkconfig.defaults: firmware must remain TLS-client-only",
    )
    require(
        sdkconfig_defaults.count("CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n") == 1
        and sdkconfig_defaults.count("CONFIG_ESP_WIFI_ENABLE_SAE_PK=n") == 1
        and sdkconfig_defaults.count("CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=n") == 1,
        "sdkconfig.defaults: unreachable enterprise/SAE-PK/setup-AP SAE surfaces must stay disabled",
    )
    require(
        idf_docker.count("docker run --rm --cpus 1.5 --memory 1800m") == 1,
        "idf-docker.sh: build containers must retain the explicit 1.5 CPU / 1800 MiB limits",
    )

    for text, label in (
        (build_all, "ci-build-all.sh"),
        (signer, "ci-sign-artifacts.sh"),
        (pages, "build-pages.sh"),
    ):
        shell_targets(text, label)

    for text, label in ((build_all, "ci-build-all.sh"), (signer, "ci-sign-artifacts.sh")):
        require(text.count("SIGNATURE_ALIGNMENT=$((0x10000))") == 1 and
                text.count("SIGNATURE_SECTOR=$((0x1000))") == 1,
                f"{label}: Secure Boot v2 projection constants must be exactly 64 KiB + 4 KiB")
    for relative in ("scripts/check-firmware-artifacts.py", "scripts/report-firmware-size.py"):
        require(literal_assignment(root, relative, "SIGNATURE_ALIGNMENT") == 0x10000 and
                literal_assignment(root, relative, "SIGNATURE_SECTOR") == 0x1000,
                f"{relative}: Secure Boot v2 projection constants drifted")

    chip_ids = literal_assignment(root, "scripts/check-firmware-artifacts.py", "CHIP_IDS")
    require(chip_ids == CHIP_IDS,
            "check-firmware-artifacts.py: exact chip-ID target mapping drifted")
    manifest_targets = literal_assignment(root, "scripts/check-pages-manifest.py", "TARGETS")
    require(manifest_targets == MANIFEST_TARGETS,
            "check-pages-manifest.py: exact target/family/layout mapping drifted")
    report_targets = literal_assignment(root, "scripts/report-firmware-size.py", "TARGETS")
    require(tuple(report_targets) == TARGETS,
            "report-firmware-size.py: target set/order drifted")
    require(literal_assignment(root, "scripts/check-build-artifact-inventory.py", "TARGETS") == TARGETS,
            "check-build-artifact-inventory.py: exact target set/order drifted")
    require(
        literal_assignment(
            root, "scripts/check-build-artifact-inventory.py", "UNSIGNED_TARGET_FILES"
        ) == UNSIGNED_TARGET_FILES
        and literal_assignment(
            root, "scripts/check-build-artifact-inventory.py", "DIST_TARGET_FILES"
        ) == DIST_TARGET_FILES
        and literal_assignment(
            root, "scripts/check-build-artifact-inventory.py", "DIST_GLOBAL_FILES"
        ) == DIST_GLOBAL_FILES
        and literal_assignment(
            root, "scripts/check-build-artifact-inventory.py", "SOURCE_EXCLUSIONS"
        ) == SOURCE_EXCLUSIONS
        and literal_assignment(
            root, "scripts/check-build-artifact-inventory.py", "EXPECTED_PAYLOAD_COUNT"
        ) == EXPECTED_PAYLOAD_COUNT,
        "check-build-artifact-inventory.py: exact payload allowlist drifted",
    )
    require(inventory.count("for target in TARGETS:") == 1,
            "check-build-artifact-inventory.py: payload construction must iterate exact TARGETS")
    require("build-seconds.txt" not in inventory and "build-seconds.txt" not in build_all,
            "artifact inventory must not contain nondeterministic build wall time")
    require("def compare_inventories(" in inventory and
            "independent artifact bytes differ" in inventory and
            "for relative in PAYLOAD_PATHS:" in inventory,
            "check-build-artifact-inventory.py: exact independent byte comparison drifted")
    merged_names = literal_assignment(
        root, "scripts/check-release-pages-bytes.py", "MERGED_NAMES"
    )
    expected_offsets = literal_assignment(
        root, "scripts/check-release-pages-bytes.py", "EXPECTED_OFFSETS"
    )
    require(merged_names == MERGED_NAMES,
            "check-release-pages-bytes.py: exact merged family/name mapping drifted")
    require(expected_offsets == EXPECTED_OFFSETS,
            "check-release-pages-bytes.py: exact family/offset mapping drifted")
    require("OTADATA_CONTRACT.validate_path(path)" in pages_manifest and
            "OTADATA_CONTRACT.validate_bytes(page_bytes" in release_pages,
            "Pages manifest/Release binding must invoke the shared otadata validator")
    require("ordered_ranges = sorted(declared_ranges)" in release_pages and
            "merged ranges overlap" in release_pages and
            "undeclared merged byte" in release_pages and
            "merged asset length must end exactly" in release_pages,
            "check-release-pages-bytes.py: complete merged prefix/gap/trailing contract drifted")
    require("merged-gap self-test failed" in build_contracts and
            "merged-length self-test failed" in build_contracts,
            "test-build-contracts.sh: merged NVS-gap/trailing mutation canaries drifted")
    require('write_bytes(b"\\xff" * int(sys.argv[2]))' in build_contracts and
            'make_ff_bytes "$stage/$target/ota_data_initial.bin" 8192' in build_contracts and
            "manifest otadata semantic self-test failed" in build_contracts,
            "test-build-contracts.sh: positive all-0xff fixture/otadata mutation canary drifted")

    require(build_all.count("for target in $TARGETS; do") == 2,
            "ci-build-all.sh: exact four-target build/budget loops drifted")
    environment_guard = re.findall(
        r'for variable in \\\n(?P<variables>.*?)\; do\n'
        r'  if declare -p "\$variable" &>/dev/null; then\n'
        r'    echo "ERROR: compiler-injection environment variable is set: \$variable" >&2\n'
        r'    exit 2\n'
        r'  fi\n'
        r'done',
        build_all,
        re.DOTALL,
    )
    require(
        len(environment_guard) == 1
        and tuple(re.findall(r"[A-Z][A-Z_]+", environment_guard[0]))
        == COMPILER_INJECTION_ENV,
        "ci-build-all.sh: exact compiler-injection environment guard drifted",
    )
    require(
        build_all.count('export EXTRA_CFLAGS="-fstack-usage"') == 1
        and build_all.count('export EXTRA_CXXFLAGS="-fstack-usage"') == 1
        and '${EXTRA_CFLAGS' not in build_all
        and '${EXTRA_CXXFLAGS' not in build_all,
        "ci-build-all.sh: stack-sidecar flags must overwrite rather than inherit caller input",
    )
    ccache_guard = (
        "caller_ccache_variables=()\n"
        "while IFS= read -r variable; do\n"
        "  [[ -n \"$variable\" ]] && caller_ccache_variables+=(\"$variable\")\n"
        "done < <(compgen -A variable CCACHE_)\n"
        "if (( ${#caller_ccache_variables[@]} != 0 )); then\n"
        "  echo \"ERROR: caller-provided ccache variables are forbidden:\" >&2\n"
        "  printf '  %s\\n' \"${caller_ccache_variables[@]}\" >&2\n"
        "  exit 2\n"
        "fi\n"
        "unset caller_ccache_variables"
    )
    require(
        build_all.count(ccache_guard) == 1
        and build_all.index(ccache_guard) < build_all.index("validate_inputs()"),
        "ci-build-all.sh: generic presence guard for every caller CCACHE_* variable drifted",
    )
    require(
        build_all.count("export IDF_CCACHE_ENABLE=0") == 1
        and not re.findall(
            r"(?<![A-Z0-9_])(?:export\s+)?CCACHE_[A-Z0-9_]+=", build_all
        )
        and build_all.index(ccache_guard)
        < build_all.index("export IDF_CCACHE_ENABLE=0")
        < build_all.index("# EXACT_FOUR_TARGETS_BEGIN build"),
        "ci-build-all.sh: authoritative builds must disable ccache exactly",
    )
    require(
        "actions/cache@" not in build_workflow
        and "cache-hash" not in build_workflow
        and "cache-scope" not in build_workflow,
        "build.yml: authoritative build must not restore or key a ccache artifact",
    )
    require(
        "def load_ninja_commands(" in build_semantics
        and '[str(ninja), "-t", "commands", "-s", output]' in build_semantics
        and "def validate_actual_compile_commands(" in build_semantics
        and "command_loader(build_root, firmware_output_names)" in build_semantics
        and "launcher before the pinned compiler" in build_semantics
        and "Ninja compile launcher mutation was accepted" in build_semantics,
        "check-build-semantics.py: actual Ninja compile-command/launcher gate drifted",
    )
    require(
        literal_assignment(
            root, "scripts/check-build-semantics.py", "BUILD_INJECTION_ENV"
        ) == COMPILER_INJECTION_ENV,
        "check-build-semantics.py: exact compiler/linker injection environment gate drifted",
    )
    build_graph_canaries = (
        "def self_test_build_graph_contract(",
        "Positive build-root -l archive fixture",
        "CMake RULE_LAUNCH_LINK canary",
        "CMake PRE_LINK canary",
        "CMake POST_BUILD canary",
        "generated .S byte-overwrite canary",
        "link launcher canary",
        "link PRE_LINK chain canary",
        "link POST_BUILD chain canary",
        "external archive canary",
        "external -L canary",
        "build-root -l shared-library canary",
        "bare linker-script canary",
        "bare shared-library canary",
        "direct --just-symbols canary",
        "direct --version-script canary",
        "combined -Wl,-l canary",
        "link response-file inventory canary",
        "linker-script indirect-input canary",
        "archive extra-object canary",
        "cross-archive duplicate-object canary",
        "altered elf2image command canary",
        "elf2image reproduced-app byte-drift canary",
        "ELF-to-app POST_BUILD chain canary",
        "built app-byte overwrite canary",
        "default-closure ALL app overwrite canary",
        "managed external include path mutation was accepted",
        "managed external dependency mutation was accepted",
        "LIBRARY_PATH environment mutation was accepted",
        "self_test_build_graph_contract(Path(__file__).resolve().parent.parent)",
    )
    require(
        all(canary in build_semantics for canary in build_graph_canaries)
        and "process_runner=reproducer(reproduced_bytes)" in build_semantics
        and "fixture_graph_validator=positive_graph_validator" in build_semantics
        and "load_ninja_default_commands(build_root)" in build_semantics,
        "check-build-semantics.py: positive build-graph fixtures/mutation canaries drifted",
    )
    require_order(
        build_all,
        "ci-build-all.sh dependency preflight",
        ("check-dependency-contract.py --root .", "check-partition-contract.py --csv partitions.csv",
         "# EXACT_FOUR_TARGETS_BEGIN build"),
    )
    require(build_all.count('-D "PROJECT_VER=$version"') == 2,
            "ci-build-all.sh: display version must bind both target configuration invocations")
    require(reproducible.count('-D "PROJECT_VER=$version"') == 2,
            "check-reproducible-build.sh: display version must bind both fresh-build invocations")
    marked_target_loop(
        build_all, "ci-build-all.sh", "build", "for target in $TARGETS; do",
        ('idf.py -D "PROJECT_VER=$version" set-target "$target"',
         'idf.py -D "PROJECT_VER=$version" build',
         "check-otadata-contract.py build/ota_data_initial.bin",
         'cp build/ota_data_initial.bin "$input/ota_data_initial.bin"'),
    )
    marked_target_loop(
        build_all, "ci-build-all.sh", "budget", "for target in $TARGETS; do",
        ("--enforce-budget", "--observed-json"),
    )
    require_order(
        build_all,
        "ci-build-all.sh target gate chain",
        (
            'idf.py -D "PROJECT_VER=$version" set-target "$target"',
            "check-sdkconfig-defaults.py",
            'idf.py -D "PROJECT_VER=$version" build',
            "check-build-semantics.py",
            "check-partition-contract.py \\",
            "check-firmware-artifacts.py \\",
            "check-stack-usage.py \\",
            "report-firmware-size.py \\",
        ),
    )
    require_order(
        build_all,
        "ci-build-all.sh source -> unsigned -> dist -> inventory chain",
        (
            'rm -rf _unsigned dist',
            'for target in $TARGETS; do',
            '} > dist/build-metadata.txt',
            'check-build-artifact-inventory.py',
            '--write --artifact-root . --source-root .',
        ),
    )

    require(signer.count("for target in $TARGETS; do") == 2,
            "ci-sign-artifacts.sh: exact four-target preflight/sign loops drifted")
    marked_target_loop(
        signer, "ci-sign-artifacts.sh", "preflight", "for target in $TARGETS; do",
        ('check-otadata-contract.py "$source_dir/ota_data_initial.bin"',
         "check-partition-contract.py", "check-firmware-artifacts.py"),
    )
    marked_target_loop(
        signer, "ci-sign-artifacts.sh", "sign", "for target in $TARGETS; do",
        ("espsecure.py sign_data", "espsecure.py verify_signature",
         'cp "$work/ota_data_initial.bin" "_fw/$target/ota_data_initial.bin"',
         'esptool.py --chip "$target" merge_bin'),
    )
    signer_root_gate = 'python3 scripts/check-signed-root-inventory.py . --version "$version"'
    require(
        signer.count(signer_root_gate) == 1
        and signer.index("# EXACT_FOUR_TARGETS_END sign")
        < signer.index(signer_root_gate)
        < signer.index('echo "Signed release artifacts:"')
        < signer.index("ls -1 tesla-key-esp32*.bin"),
        "ci-sign-artifacts.sh: exact signed root inventory gate must follow all targets before listing",
    )
    key_boundary = signer.find("signing_key=")
    require(key_boundary > 0, "ci-sign-artifacts.sh: signing-key boundary is missing")
    key_free = signer[:key_boundary]
    require("check-build-artifact-inventory.py" in key_free and
            'check-dependency-contract.py --root "$source_root"' in key_free and
            '--copy-to "$private_stage"' in key_free and
            '--compare-to "$independent_root"' in key_free and
            '--expected-source-sha "$expected_source_sha"' in key_free and
            'unsigned_dir="$private_stage/_unsigned"' in key_free and
            "check-partition-contract.py" in key_free and
            "check-firmware-artifacts.py" in key_free and
            "check-otadata-contract.py" in key_free,
            "ci-sign-artifacts.sh: independent inventory/private-stage preflight drifted")
    sign_body = signer[key_boundary:]
    require("$unsigned_input" not in sign_body and "$artifact_root" not in sign_body,
            "ci-sign-artifacts.sh: key-bearing phase must never reread producer-owned paths")
    require(
        re.search(
            r"espsecure\.py sign_data\b[\s\S]*?\n\s*fi\n"
            r"\s*if ! espsecure\.py verify_signature\b",
            sign_body,
        )
        is not None,
        "ci-sign-artifacts.sh: signer must directly verify_signature after sign_data",
    )
    require_order(
        sign_body,
        "ci-sign-artifacts.sh trusted signer chain",
        (
            "espsecure.py sign_data",
            "espsecure.py verify_signature",
            "expected_signed_size=",
            "signed_size == expected_signed_size",
            "--signed-app",
            "esptool.py --chip",
        ),
    )
    require("SIGNATURE_ALIGNMENT" in sign_body and "SIGNATURE_SECTOR" in sign_body,
            "ci-sign-artifacts.sh: exact Secure Boot v2 size projection is missing")

    require(verify.count(EXACT_TARGET_LOOP) == 1,
            "ci-build-verify.sh: reproducibility loop must cover exactly four targets")
    marked_target_loop(
        verify, "ci-build-verify.sh", "repro", EXACT_TARGET_LOOP,
        ('./scripts/check-reproducible-build.sh "$target"', '"$version"',),
    )
    require_order(
        verify,
        "ci-build-verify.sh build -> sign -> reproducibility chain",
        ("CJSON_OOM_SANITIZE=1 bash ./test/run-cjson-oom-tests.sh",
         "MQTT_JSON_SANITIZE=1 bash ./test/run-mqtt-json-publish-tests.sh",
         "./scripts/ci-build-all.sh", "check-build-artifact-inventory.py",
         "./scripts/test-release-contract.sh", EXACT_TARGET_LOOP,
         "./scripts/check-reproducible-build.sh"),
    )
    for text, label, variable in (
        (cjson_test, "run-cjson-oom-tests.sh", "CJSON_OOM_SANITIZE"),
        (mqtt_json_test, "run-mqtt-json-publish-tests.sh", "MQTT_JSON_SANITIZE"),
    ):
        require(': "${IDF_PATH:?' in text and
                '$IDF_PATH/components/json/cJSON' in text and
                '"v5.5.5"' in text and variable in text,
                f"{label}: exact ESP-IDF v5.5.5 cJSON/sanitizer contract drifted")
    require(release_test.count(EXACT_TARGET_LOOP) == 1,
            "test-release-contract.sh: signer verification loop must cover exactly four targets")
    marked_target_loop(
        release_test, "test-release-contract.sh", "verify-signed", EXACT_TARGET_LOOP,
        ("espsecure.py verify_signature", 'check-otadata-contract.py "_fw/$target/ota_data_initial.bin"'),
    )
    require("tampered_signed=" in release_test and
            "if espsecure.py verify_signature" in release_test and
            "data[64] ^= 0x01" in release_test,
            "test-release-contract.sh: cryptographic tampering canary is missing")
    require_order(
        release_test,
        "test-release-contract.sh signed Pages chain",
        ("./scripts/ci-sign-artifacts.sh", EXACT_TARGET_LOOP, "tampered_signed=",
         "./scripts/build-pages.sh", "check-release-pages-bytes.py"),
    )
    marked_target_loop(
        pages, "build-pages.sh", "pages", "for target in $TARGETS; do",
        ('check-otadata-contract.py" "$source_dir/ota_data_initial.bin"',
         'cp "$source_dir/ota_data_initial.bin" "$out/$otadata_name"'),
    )
    require(pages.count("for target in $TARGETS; do") == 1,
            "build-pages.sh: exact four-target Pages loop drifted")

    require("fullclean" not in reproducible and "|| true" not in reproducible,
            "check-reproducible-build.sh: cleanup/build failures must not be ignored")
    require('build_dir="$work_root/build-$pass"' in reproducible and
            '[[ ! -e "$build_dir" && ! -e "$generated_config" ]]' in reproducible and
            reproducible.count('idf.py -B "$build_dir"') == 2 and
            'SDKCONFIG=$generated_config' in reproducible,
            "check-reproducible-build.sh: each pass must use a distinct fresh -B/SDKCONFIG path")
    require("temporary workspace cleanup failed" in reproducible and
            'rm -rf -- "$work_root"' in reproducible,
            "check-reproducible-build.sh: temporary cleanup must fail closed")

    require(release_assets.count('release.get("immutable") is not True') == 1 and
            '(("false", False), ("missing", None))' in release_assets,
            "check-release-assets.py: immutable true gate/canaries drifted")
    require(
        literal_assignment(
            root, "scripts/check-release-assets.py", "EXPECTED_FULL_ASSET_COUNT"
        ) == 40
        and "def validate_full_release(" in release_assets
        and "set(names) != expected_names" in release_assets
        and "GitHub Release full asset inventory drifted" in release_assets
        and "--expect-state" in release_assets
        and "incomplete draft Release asset set was accepted" in release_assets
        and "extra draft Release asset was accepted" in release_assets,
        "check-release-assets.py: exact 40-file draft/published byte contract drifted",
    )
    require(
        "def validate_staged_merged_layout(" in release_assets
        and "staged partition table" in release_assets
        and "staged otadata" in release_assets
        and "undeclared merged byte" in release_assets
        and "merged image has trailing or missing bytes" in release_assets
        and '("bootloader", lambda data:' in release_assets
        and '("partition table", lambda data:' in release_assets
        and '("otadata", lambda data:' in release_assets
        and '("undeclared merged byte", lambda data:' in release_assets
        and "pre-publication {label} mutation was accepted" in release_assets
        and "pre-publication merged trailing bytes were accepted" in release_assets,
        "check-release-assets.py: complete pre-publication merged-layout gate drifted",
    )
    require(
        build_workflow.count("--expected-public-key-digest scripts/ota-signing-public-key.sha256")
        == 4
        and "Verify production signing authority for all staged apps" in build_workflow
        and "      - name: Upload signed firmware artifacts\n        uses:" in build_workflow
        and build_workflow.index("Verify production signing authority for all staged apps")
        < build_workflow.index("Upload signed firmware artifacts")
        and build_workflow.index("--expect-state draft")
        < build_workflow.index(PUBLISH_DRAFT),
        "build.yml: production-key and complete draft validation must precede publication/upload",
    )
    require(
        preview_workflow.count(
            "--expected-public-key-digest scripts/ota-signing-public-key.sha256"
        )
        == 1
        and "Verify production signing authority for all preview apps" in preview_workflow
        and preview_workflow.index("./scripts/ci-sign-artifacts.sh")
        < preview_workflow.index("Verify production signing authority for all preview apps")
        < preview_workflow.index("Assemble signed PR preview site")
        < preview_workflow.index("Upload current-head signed hardware-test artifact"),
        "signed-pr-preview.yml: production-key verification must cover all four apps before publication/upload",
    )
    main_root_paths = Counter(
        {
            template.format(version="${{ needs.build.outputs.display-version }}"): 2
            for template in SIGNED_ROOT_TEMPLATES
        }
    )
    preview_root_paths = Counter(
        {
            template.format(version="${{ steps.metadata.outputs.version }}"): 1
            for template in SIGNED_ROOT_TEMPLATES
        }
    )
    require(
        workflow_signed_root_paths(build_workflow) == main_root_paths
        and build_workflow.count("python3 scripts/check-signed-root-inventory.py") == 3
        and "tesla-key-esp32*.bin" not in build_workflow,
        "build.yml: exact 12-file signed root gates/listings must protect Actions, Draft and deploy",
    )
    require(
        workflow_signed_stage_paths(build_workflow)
        == Counter({path: 1 for path in SIGNED_STAGE_PATHS}),
        "build.yml: signed Actions artifact must explicitly carry the exact 16 signer layout inputs",
    )
    require(
        build_workflow.count(
            "name: tesla-key-esp32-${{ needs.build.outputs.display-version }}-${{ github.sha }}"
        ) == 2,
        "build.yml: publish and deploy must share the exact SHA/version-bound artifact name",
    )
    require(
        workflow_signed_root_paths(preview_workflow) == preview_root_paths
        and preview_workflow.count("python3 scripts/check-signed-root-inventory.py") == 1
        and "tesla-key-esp32*.bin" not in preview_workflow,
        "signed-pr-preview.yml: exact 12-file signed root gate/listing must protect upload",
    )
    require(release_relevance.count(".immutable == true") == 1 and
            "mutable Release authority failed open" in release_relevance and
            "without immutable field failed open" in release_relevance,
            "release-relevance.sh: immutable true authority/canaries drifted")
    require(
        release_relevance.count("prepare-reused-release") == 3
        and "immutable Release reuse contract change was not release-relevant"
        in release_relevance,
        "release-relevance.sh: immutable Release reuse path/canary drifted",
    )
    require(release_selector.count(".immutable == true") == 3 and
            "fake-release-without-immutable.json" in release_selector and
            "fake-release-mutable.json" in release_selector,
            "select-release-version.sh: all Release API authorities must require immutable true")
    require('parser.add_argument("--release-json", type=Path)' in published_release and
            "release_validator.validate_metadata(release_metadata" in published_release and
            "release_validator.validate(release_metadata" in published_release and
            '(("false", False), ("missing", None))' in published_release,
            "check-published-release.py: immutable API metadata gate/canaries drifted")

    require(build_workflow.count(HEAD_REF) == 1 and SOURCE_EXPR in build_workflow,
            "build.yml: PR producer must bind to the exact head SHA")
    require(
        build_workflow.count("  independent-rebuild:") == 1
        and build_workflow.count("  publish:") == 1
        and build_workflow.count("  deploy:") == 1
        and "needs: [build, independent-rebuild]" in build_workflow
        and "needs: [build, publish]" in build_workflow
        and "if: github.event_name == 'push' && github.ref == 'refs/heads/main'"
        in build_workflow
        and "ref: ${{ github.sha }}" in build_workflow
        and "name: firmware-independent-rebuild" in build_workflow
        and "Independently rebuild all four targets" in build_workflow
        and "./scripts/ci-build-all.sh" in build_workflow
        and "--compare-to _ci-independent" in build_workflow,
        "build.yml: logic/build/rebuild/publish/deploy chain or independent byte comparison drifted",
    )
    require("python3 scripts/check-build-gate-contract.py --self-test" in build_workflow,
            "build.yml: static build-gate contract is not wired into logic-test")
    require_order(
        build_workflow,
        "build.yml draft -> immutable Release transaction",
        ("Assemble local Pages candidate before any publication", LOCAL_PAGES_CHECK,
         "Upload signed firmware artifacts", "Create draft release", DRAFT_RELEASE_API,
         DRAFT_RELEASE_CHECK, PUBLISH_DRAFT, "Refetch and accept immutable published Release"),
    )
    refetch_position = build_workflow.find("Refetch and accept immutable published Release")
    immutable_position = build_workflow.find(IMMUTABLE_RELEASE_CHECK, refetch_position)
    deploy_position = build_workflow.find("  deploy:")
    require(refetch_position < immutable_position < deploy_position,
            "build.yml publish must accept the immutable Release before deploy starts")
    require(
        "draft: true" in build_workflow
        and "fail_on_unmatched_files: true" in build_workflow
        and "id: create-release" in build_workflow
        and "DRAFT_RELEASE_ID: ${{ steps.create-release.outputs.id }}" in build_workflow
        and '[[ "$DRAFT_RELEASE_ID" =~ ^[1-9][0-9]*$ ]]' in build_workflow
        and '[[ "$release_id" == "$DRAFT_RELEASE_ID" ]]' in build_workflow
        and build_workflow.count(
            'gh api "repos/$GITHUB_REPOSITORY/releases/tags/v$RELEASE_VERSION"'
        ) >= 3,
        "build.yml must draft-upload, exact-byte validate, publish and freshly refetch Release",
    )
    publish_section = build_workflow[build_workflow.index("  publish:"):deploy_position]
    deploy_section = build_workflow[deploy_position:]
    root_publish = "./scripts/publish-pages-branch.sh root"
    require(
        root_publish not in publish_section
        and "OTA_SIGNING_KEY" not in deploy_section
        and "environment: firmware-signing" not in deploy_section
        and "id-token" not in deploy_section,
        "build.yml must separate Release/signing authority from branch-only deploy authority",
    )
    require_order(
        publish_section,
        "build.yml local Pages candidate before first external publication",
        ("./scripts/ci-sign-artifacts.sh", LOCAL_PAGES_CHECK,
         "Revalidate current Release candidate immediately before signed artifact upload",
         "python3 scripts/check-signed-root-inventory.py .",
         "Upload signed firmware artifacts", "Create draft release"),
    )
    release_mutation_start = publish_section.index(
        "Revalidate current Release candidate immediately before Release mutation"
    )
    release_mutation_end = publish_section.index("Create draft release", release_mutation_start)
    require(
        "python3 scripts/check-signed-root-inventory.py ."
        in publish_section[release_mutation_start:release_mutation_end],
        "build.yml Draft upload must be preceded by the exact signed root inventory gate",
    )
    require(
        build_workflow.count(PAGES_SOURCE_CHECK) == 3
        and publish_section.count(PAGES_SOURCE_CHECK) == 1
        and deploy_section.count(PAGES_SOURCE_CHECK) == 2
        and "Revalidate current Release candidate immediately before signed artifact upload"
        in build_workflow
        and "actions/configure-pages@" not in build_workflow
        and "actions/upload-pages-artifact@" not in build_workflow
        and "actions/deploy-pages@" not in build_workflow,
        "build.yml must revalidate signed uploads and use only branch-backed Pages authority",
    )
    require_order(
        deploy_section,
        "build.yml deploy artifact -> authority -> branch -> live acceptance",
        ("Download exact signed Release and Pages candidate",
         "python3 scripts/check-signed-root-inventory.py ./_deploy-input",
         "python3 scripts/check-pages-manifest.py ./_deploy-input/_site",
         LOCAL_PAGES_CHECK,
         "Revalidate branch-backed Pages authority immediately before deployment",
         'python3 scripts/check-release-assets.py "$release_json" ./_deploy-input',
         root_publish, "Accept branch-served Pages against immutable Release bytes"),
    )
    final_release_position = deploy_section.rfind(FINAL_RELEASE_API)
    require(
        final_release_position > deploy_section.find(root_publish)
        and deploy_section.find(FINAL_RELEASE_CHECK, final_release_position)
        > final_release_position
        and deploy_section.find(FINAL_RELEASE_JSON_ARG, final_release_position)
        > final_release_position,
        "build.yml deploy live acceptance must fetch fresh immutable Release metadata after branch publication",
    )
    require(
        "ref: ${{ github.sha }}" in deploy_section
        and "persist-credentials: false" in deploy_section
        and "name: tesla-key-esp32-${{ needs.build.outputs.display-version }}-${{ github.sha }}"
        in deploy_section
        and "path: _deploy-input" in deploy_section,
        "build.yml deploy must use exact-SHA checkout and exact SHA/version-bound artifact",
    )
    require(
        "--expect-state published-immutable" in deploy_section
        and "--expected-public-key-digest scripts/ota-signing-public-key.sha256"
        in deploy_section
        and (
            "--expected-public-key-digest scripts/ota-signing-public-key.sha256\n\n"
            "      - name: Deploy root site to gh-pages"
        ) in deploy_section,
        "build.yml deploy must fully byte-bind its artifact to the immutable Release immediately before branch mutation",
    )
    require_order(
        preview_workflow,
        "signed-pr-preview.yml signed Pages publication",
        ("./scripts/ci-sign-artifacts.sh",
         "Verify production signing authority for all preview apps", LOCAL_PAGES_CHECK,
         "Revalidate PR immediately before signed artifact upload",
         "python3 scripts/check-signed-root-inventory.py .",
         "Upload current-head signed hardware-test artifact",
         "Revalidate PR immediately before publication",
         "./scripts/publish-pages-branch.sh pr"),
    )
    require(preview_workflow.count(PAGES_SOURCE_CHECK) == 2,
            "signed-pr-preview.yml must validate branch-backed Pages before key and branch mutation")
    require(
        "  workflow_run:" in preview_workflow
        and "workflows: [build]" in preview_workflow
        and "  trusted-rebuild:" in preview_workflow
        and "needs: [validate, trusted-rebuild]" in preview_workflow
        and "ref: ${{ github.sha }}" in preview_workflow
        and "ref: ${{ needs.validate.outputs.head-sha }}\n          fetch-depth: 0"
        in preview_workflow
        and preview_workflow.count("ref: ${{ needs.validate.outputs.head-sha }}") == 1
        and "path: _ci-source" not in preview_workflow
        and "--source-root _ci-source" not in preview_workflow
        and "--compare-to _ci-independent" in preview_workflow
        and "--accept-workflow-attested-source" in preview_workflow
        and "--workflow-attested _ci-independent" in preview_workflow
        and preview_workflow.count(
            "name: trusted-preview-rebuild-${{ needs.validate.outputs.head-sha }}"
        ) == 2
        and "name: firmware-independent-rebuild" not in preview_workflow
        and "./scripts/select-release-version.sh --latest-published-stable"
        in preview_workflow
        and 'if [ "$version" != "$EXPECTED_VERSION" ]; then' in preview_workflow
        and "_ci-source/" not in preview_workflow
        and "cd _ci-source" not in preview_workflow
        and "check-build-artifact-inventory.py" in preview_workflow,
        "signed-pr-preview.yml must use a default-owned rebuild and exact stable-base byte binding",
    )
    require_order(
        preview_workflow,
        "signed-pr-preview.yml trusted rebuild -> protected compare -> key chain",
        ("  trusted-rebuild:", "Independently rebuild exact PR head",
         "Upload trusted independent rebuild evidence", "  sign-preview:",
         "Download trusted independent PR rebuild as data",
         "check-build-artifact-inventory.py",
         "Revalidate PR immediately before key provisioning", "Provision OTA signing key"),
    )
    require(
        preview_workflow.count(TRUSTED_DEFAULT_ENV) == 3
        and preview_workflow.count(TRUSTED_DEFAULT_FETCH) == 3
        and preview_workflow.count(TRUSTED_DEFAULT_COMPARE) == 3,
        "signed-pr-preview.yml must bind key use, artifact upload and Pages publication "
        "to the current trusted default SHA",
    )
    preview_default_boundaries = (
        ("Revalidate PR immediately before key provisioning", "Provision OTA signing key"),
        ("Revalidate PR immediately before signed artifact upload",
         "Upload current-head signed hardware-test artifact"),
        ("Revalidate PR immediately before publication", "Publish signed PR preview to gh-pages"),
    )
    for begin, end in preview_default_boundaries:
        start = preview_workflow.find(begin)
        finish = preview_workflow.find(end, start + 1)
        segment = preview_workflow[start:finish]
        require(
            start >= 0 and finish > start
            and TRUSTED_DEFAULT_ENV in segment
            and TRUSTED_DEFAULT_FETCH in segment
            and TRUSTED_DEFAULT_COMPARE in segment,
            f"signed-pr-preview.yml {begin}: current-default binding is missing before mutation",
        )
    require(
        "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}-${{ needs.validate.outputs.head-sha }}"
        in preview_workflow,
        "signed-pr-preview.yml signed artifact name must bind the full current head SHA",
    )
    require_order(
        build_workflow,
        "build.yml protected inventory verification",
        ("Validate build provenance", "check-build-artifact-inventory.py",
         "Provision OTA signing key", "./scripts/ci-sign-artifacts.sh"),
    )


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    require(text.count(old) >= 1,
            f"self-test fixture text is absent from {path.relative_to(path.parents[2])}: {old}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def copy_fixture(root: Path, destination: Path) -> None:
    for relative in REQUIRED_FILES:
        source = root / relative
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def self_test(root: Path) -> None:
    validate(root)
    mutations = (
        ("idf-docker-resource-limits", "scripts/idf-docker.sh",
         "docker run --rm --cpus 1.5 --memory 1800m",
         "docker run --rm --cpus 4 --memory 8g", "explicit 1.5 CPU / 1800 MiB limits"),
        ("build-target", "scripts/ci-build-all.sh",
         'TARGETS="esp32 esp32s3 esp32c3 esp32c6"',
         'TARGETS="esp32 esp32s3 esp32c3"', "ci-build-all.sh: target set/order"),
        ("compiler-environment-guard", "scripts/ci-build-all.sh",
         "  CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH OBJC_INCLUDE_PATH \\\n",
         "  CPLUS_INCLUDE_PATH C_INCLUDE_PATH OBJC_INCLUDE_PATH \\\n",
         "exact compiler-injection environment guard"),
        ("stack-flags-overwrite", "scripts/ci-build-all.sh",
         'export EXTRA_CFLAGS="-fstack-usage"',
         'export EXTRA_CFLAGS="${EXTRA_CFLAGS:-} -fstack-usage"',
         "stack-sidecar flags must overwrite"),
        ("ccache-presence-guard", "scripts/ci-build-all.sh",
         "compgen -A variable CCACHE_", "compgen -A variable CCACHE_PREFIX",
         "generic presence guard for every caller CCACHE_*"),
        ("ccache-disabled", "scripts/ci-build-all.sh",
         "export IDF_CCACHE_ENABLE=0", "export IDF_CCACHE_ENABLE=1",
         "must disable ccache exactly"),
        ("canonical-version-grammar", "scripts/ci-sign-artifacts.sh",
         SHELL_VERSION_ASSIGNMENT,
         "VERSION_RE='^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?$'",
         "canonical display-version grammar"),
        ("actual-ninja-command", "scripts/check-build-semantics.py",
         '[str(ninja), "-t", "commands", "-s", output]',
         '[str(ninja), "-t", "commands", output]',
         "actual Ninja compile-command/launcher gate"),
        ("ninja-launcher-canary", "scripts/check-build-semantics.py",
         "Ninja compile launcher mutation was accepted",
         "Ninja compile launcher mutation was disabled",
         "actual Ninja compile-command/launcher gate"),
        ("semantics-library-path-guard", "scripts/check-build-semantics.py",
         '    "COMPILER_PATH", "LIBRARY_PATH",',
         '    "COMPILER_PATH",',
         "exact compiler/linker injection environment gate"),
        ("positive-build-graph-fixture", "scripts/check-build-semantics.py",
         "def self_test_build_graph_contract(",
         "def removed_self_test_build_graph_contract(",
         "positive build-graph fixtures/mutation canaries"),
        ("default-closure-overwrite-canary", "scripts/check-build-semantics.py",
         "default-closure ALL app overwrite canary",
         "default app overwrite canary disabled",
         "positive build-graph fixtures/mutation canaries"),
        ("elf2image-byte-reproduction", "scripts/check-build-semantics.py",
         "process_runner=reproducer(reproduced_bytes)",
         "process_runner=subprocess.run",
         "positive build-graph fixtures/mutation canaries"),
        ("sign-target", "scripts/ci-sign-artifacts.sh",
         'TARGETS="esp32 esp32s3 esp32c3 esp32c6"',
         'TARGETS="esp32 esp32s3 esp32c3"', "ci-sign-artifacts.sh: target set/order"),
        ("pages-target", "scripts/build-pages.sh",
         'TARGETS="esp32 esp32s3 esp32c3 esp32c6"',
         'TARGETS="esp32 esp32s3 esp32c3"', "build-pages.sh: target set/order"),
        ("dependency-target", "scripts/check-dependency-contract.py",
         'TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")',
         'TARGETS = ("esp32", "esp32s3", "esp32c3")', "exact target lock set/order"),
        ("otadata-size", "scripts/check-otadata-contract.py", "OTADATA_SIZE = 0x2000",
         "OTADATA_SIZE = 0x1000", "exact 0x2000/all-0xff"),
        ("otadata-erased", "scripts/check-otadata-contract.py", "ERASED_BYTE = 0xFF",
         "ERASED_BYTE = 0x00", "exact 0x2000/all-0xff"),
        ("dependency-build-wiring", "scripts/ci-build-all.sh",
         "python3 scripts/check-dependency-contract.py --root .",
         "python3 scripts/missing-dependency-contract.py --root .", "required stage is missing"),
        ("dependency-signer-wiring", "scripts/ci-sign-artifacts.sh",
         'python3 scripts/check-dependency-contract.py --root "$source_root"',
         'python3 scripts/missing-dependency-contract.py --root "$source_root"',
         "independent inventory/private-stage preflight"),
        ("otadata-build-wiring", "scripts/ci-build-all.sh",
         "python3 scripts/check-otadata-contract.py build/ota_data_initial.bin",
         "python3 scripts/missing-otadata-contract.py build/ota_data_initial.bin",
         "loop must reach exactly one"),
        ("otadata-signer-wiring", "scripts/ci-sign-artifacts.sh",
         'python3 scripts/check-otadata-contract.py "$source_dir/ota_data_initial.bin"',
         'python3 scripts/missing-otadata-contract.py "$source_dir/ota_data_initial.bin"',
         "loop must reach exactly one"),
        ("otadata-pages-wiring", "scripts/build-pages.sh",
         'check-otadata-contract.py" "$source_dir/ota_data_initial.bin"',
         'missing-otadata-contract.py" "$source_dir/ota_data_initial.bin"',
         "loop must reach exactly one"),
        ("otadata-manifest-wiring", "scripts/check-pages-manifest.py",
         "OTADATA_CONTRACT.validate_path(path)", "pass  # removed shared otadata validation",
         "must invoke the shared otadata validator"),
        ("otadata-merged-wiring", "scripts/check-release-pages-bytes.py",
         "OTADATA_CONTRACT.validate_bytes(page_bytes",
         "fixture_otadata_accepts_bytes(page_bytes", "must invoke the shared otadata validator"),
        ("merged-gap-gate", "scripts/check-release-pages-bytes.py",
         "ordered_ranges = sorted(declared_ranges)", "ordered_ranges = []",
         "complete merged prefix/gap/trailing"),
        ("merged-trailing-gate", "scripts/check-release-pages-bytes.py",
         "merged asset length must end exactly", "merged asset may have trailing bytes",
         "complete merged prefix/gap/trailing"),
        ("otadata-positive-fixture", "scripts/test-build-contracts.sh",
         'make_ff_bytes "$stage/$target/ota_data_initial.bin" 8192',
         'make_bytes "$stage/$target/ota_data_initial.bin" 8192',
         "positive all-0xff fixture"),
        ("otadata-manifest-canary", "scripts/test-build-contracts.sh",
         "manifest otadata semantic self-test failed", "manifest otadata mutation disabled",
         "positive all-0xff fixture"),
        ("merged-gap-canary", "scripts/test-build-contracts.sh",
         "merged-gap self-test failed", "merged-gap mutation disabled",
         "merged NVS-gap/trailing"),
        ("merged-trailing-canary", "scripts/test-build-contracts.sh",
         "merged-length self-test failed", "merged-length mutation disabled",
         "merged NVS-gap/trailing"),
        ("chip-id", "scripts/check-firmware-artifacts.py", '"esp32c6": 0x000D',
         '"esp32c6": 0x000C', "exact chip-ID"),
        ("production-key-pin", "scripts/ota-signing-public-key.sha256",
         "0cfdce59b489a8a1a285a4f5b39b4332061d6f649ae7948d3804279378381889",
         "1cfdce59b489a8a1a285a4f5b39b4332061d6f649ae7948d3804279378381889",
         "public-key digest pin drifted"),
        ("rsa-pss-verifier", "scripts/check-firmware-artifacts.py",
         "def verify_rsa_pss_sha256(", "def removed_rsa_pss_verifier(",
         "RSA-PSS/public-key authority"),
        ("app-only-production-verifier", "scripts/check-firmware-artifacts.py",
         'parser.add_argument("--app-only", action="store_true")',
         'parser.add_argument("--unchecked-app-only", action="store_true")',
         "RSA-PSS/public-key authority"),
        ("reuse-production-key", "scripts/prepare-reused-release.py",
         "expected_public_key_digest=expected_public_key_digest",
         "expected_public_key_digest=None", "production-key/recovery staging"),
        ("reuse-recovery-root", "scripts/prepare-reused-release.py",
         "valid fixture did not stage twelve root recovery artifacts",
         "root recovery artifact canary removed", "production-key/recovery staging"),
        ("reuse-snapshot-nofollow", "scripts/prepare-reused-release.py",
         "os.O_RDONLY | os.O_NOFOLLOW", "os.O_RDONLY | 0",
         "no-follow immutable Release snapshot"),
        ("reuse-post-metadata-path-swap", "scripts/prepare-reused-release.py",
         "after_snapshot_validation=replace_download_paths_after_metadata",
         "after_snapshot_validation=None",
         "post-metadata path-swap snapshot canary"),
        ("reuse-release-relevance", "scripts/release-relevance.sh",
         "check-signed-root-inventory|prepare-reused-release|check-published-release",
         "check-signed-root-inventory|check-published-release",
         "immutable Release reuse path/canary"),
        ("prepublish-merged-layout", "scripts/check-release-assets.py",
         "def validate_staged_merged_layout(", "def removed_staged_merged_layout(",
         "complete pre-publication merged-layout"),
        ("tls-client-only", "sdkconfig.defaults",
         "CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y", "CONFIG_MBEDTLS_TLS_CLIENT_ONLY=n",
         "firmware must remain TLS-client-only"),
        ("wifi-enterprise-disabled", "sdkconfig.defaults",
         "CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=n", "CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT=y",
         "unreachable enterprise/SAE-PK/setup-AP SAE surfaces"),
        ("wifi-sae-pk-disabled", "sdkconfig.defaults",
         "CONFIG_ESP_WIFI_ENABLE_SAE_PK=n", "CONFIG_ESP_WIFI_ENABLE_SAE_PK=y",
         "unreachable enterprise/SAE-PK/setup-AP SAE surfaces"),
        ("wifi-softap-sae-disabled", "sdkconfig.defaults",
         "CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=n", "CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=y",
         "unreachable enterprise/SAE-PK/setup-AP SAE surfaces"),
        ("boundary-build-ignore", ".gitignore", "/build_boundary_*/", "/build_boundary_removed/",
         "boundary-build trees must stay outside the source fingerprint"),
        ("manifest-layout", "scripts/check-pages-manifest.py",
         '("ESP32-C6", "esp32c6", "-c6", 0)',
         '("ESP32-C6", "esp32c6", "-c6", 0x1000)', "target/family/layout"),
        ("inventory-target", "scripts/check-build-artifact-inventory.py",
         'TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")',
         'TARGETS = ("esp32", "esp32s3", "esp32c3")', "exact target set/order"),
        ("inventory-path", "scripts/check-build-artifact-inventory.py",
         '"ota_data_initial.bin",', '"ota-data.bin",', "exact payload allowlist"),
        ("inventory-source-exclusion", "scripts/check-build-artifact-inventory.py",
         'SOURCE_EXCLUSIONS = ("version.txt",)',
         'SOURCE_EXCLUSIONS = ("version.txt", "main/")', "exact payload allowlist"),
        ("inventory-build-wiring", "scripts/ci-build-all.sh",
         'python3 scripts/check-build-artifact-inventory.py',
         'python3 scripts/missing-inventory.py', "required stage is missing"),
        ("inventory-private-copy", "scripts/ci-sign-artifacts.sh",
         '--copy-to "$private_stage"', '--verify', "private-stage preflight"),
        ("inventory-independent-compare", "scripts/ci-sign-artifacts.sh",
         '--compare-to "$independent_root"', '--compare-to "$artifact_root"',
         "independent inventory/private-stage preflight"),
        ("repro-target", "scripts/ci-build-verify.sh", EXACT_TARGET_LOOP,
         "for target in esp32 esp32s3 esp32c3; do", "reproducibility loop"),
        ("primary-version-binding", "scripts/ci-build-all.sh",
         '-D "PROJECT_VER=$version" set-target', 'set-target',
         "display version must bind"),
        ("repro-version-binding", "scripts/check-reproducible-build.sh",
         '-D "PROJECT_VER=$version" set-target', 'set-target',
         "display version must bind"),
        ("repro-shared-dir", "scripts/check-reproducible-build.sh",
         'build_dir="$work_root/build-$pass"', 'build_dir="$work_root/build"',
         "distinct fresh"),
        ("ignored-cleanup", "scripts/check-reproducible-build.sh",
         'rm -rf -- "$work_root"', 'rm -rf -- "$work_root" || true',
         "must not be ignored"),
        ("signer-verify", "scripts/ci-sign-artifacts.sh", "espsecure.py verify_signature",
         "espsecure.py digest_data", "loop must reach exactly one"),
        ("signer-root-inventory", "scripts/ci-sign-artifacts.sh",
         'python3 scripts/check-signed-root-inventory.py . --version "$version"',
         'python3 scripts/missing-signed-root-inventory.py . --version "$version"',
         "exact signed root inventory gate"),
        ("signer-alignment", "scripts/ci-sign-artifacts.sh",
         "SIGNATURE_ALIGNMENT=$((0x10000))", "SIGNATURE_ALIGNMENT=$((0x20000))",
         "exactly 64 KiB + 4 KiB"),
        ("release-negative", "scripts/test-release-contract.sh", "data[64] ^= 0x01",
         "data[64] ^= 0x00", "tampering canary"),
        ("release-immutable", "scripts/check-release-assets.py",
         'if release.get("immutable") is not True:', "if False:", "immutable true gate"),
        ("release-full-count", "scripts/check-release-assets.py",
         "EXPECTED_FULL_ASSET_COUNT = 40", "EXPECTED_FULL_ASSET_COUNT = 39",
         "exact 40-file"),
        ("workflow-production-key", ".github/workflows/build.yml",
         "Verify production signing authority for all staged apps",
         "Trust unverified staged signing authority",
         "production-key and complete draft validation"),
        ("preview-production-key", ".github/workflows/signed-pr-preview.yml",
         "--expected-public-key-digest scripts/ota-signing-public-key.sha256",
         "--expected-public-key-digest scripts/unreviewed-preview-key.sha256",
         "production-key verification must cover all four apps"),
        ("preview-current-default", ".github/workflows/signed-pr-preview.yml",
         TRUSTED_DEFAULT_ENV, "TRUSTED_DEFAULT_SHA: ${{ needs.validate.outputs.head-sha }}",
         "current trusted default SHA"),
        ("relevance-immutable", "scripts/release-relevance.sh", ".immutable == true",
         ".immutable == false", "immutable true authority"),
        ("selector-immutable", "scripts/select-release-version.sh", ".immutable == true",
         ".immutable == false", "all Release API authorities"),
        ("acceptance-immutable", "scripts/check-published-release.py",
         "release_validator.validate_metadata(release_metadata",
         "release_validator.validate_metadata({}", "immutable API metadata gate"),
        ("pr-head", ".github/workflows/build.yml", HEAD_REF,
         "ref: ${{ github.sha }}", "exact head SHA"),
        ("independent-rebuild", ".github/workflows/build.yml",
         "  independent-rebuild:", "  skipped-rebuild:",
         "logic/build/rebuild/publish/deploy chain"),
        ("independent-compare", ".github/workflows/build.yml",
         "--compare-to _ci-independent", "--compare-to .",
         "logic/build/rebuild/publish/deploy chain"),
        ("release-draft", ".github/workflows/build.yml", "          draft: true\n",
         "          draft: false\n", "draft-upload"),
        ("release-draft-action-id", ".github/workflows/build.yml",
         "        id: create-release\n", "", "draft-upload"),
        ("release-draft-output-id", ".github/workflows/build.yml",
         "DRAFT_RELEASE_ID: ${{ steps.create-release.outputs.id }}",
         "DRAFT_RELEASE_ID: ${{ steps.create-release.outputs.url }}", "draft-upload"),
        ("release-draft-by-tag", ".github/workflows/build.yml", DRAFT_RELEASE_API,
         'gh api "repos/$GITHUB_REPOSITORY/releases/tags/v$RELEASE_VERSION" > "$draft_json"',
         "draft -> immutable Release transaction"),
        ("release-publish-order", ".github/workflows/build.yml", DRAFT_RELEASE_CHECK,
         PUBLISH_DRAFT + "\n            " + DRAFT_RELEASE_CHECK,
         "production-key and complete draft validation"),
        ("release-final-state", ".github/workflows/build.yml",
         "            --expect-state published-immutable \\\n"
         "            --expected-public-key-digest scripts/ota-signing-public-key.sha256\n\n"
         "  deploy:",
         "            --expect-state draft \\\n"
         "            --expected-public-key-digest scripts/ota-signing-public-key.sha256\n\n"
         "  deploy:",
         "accept the immutable Release before deploy"),
        ("final-release-json", ".github/workflows/build.yml", FINAL_RELEASE_JSON_ARG,
         '--release-json "$stale_release_json"', "deploy live acceptance"),
        ("pages-source-selftest-wiring", "scripts/test-build-contracts.sh",
         'python3 "$repo_root/scripts/check-pages-source.py" --self-test\n', "",
         "Pages source self-test is not wired"),
        ("release-reuse-selftest-wiring", "scripts/test-build-contracts.sh",
         'python3 "$repo_root/scripts/prepare-reused-release.py" --self-test\n', "",
         "immutable Release reuse self-test is not wired"),
        ("release-reuse-host-wiring", "scripts/run-mock-tests.sh",
         "python3 scripts/prepare-reused-release.py --self-test",
         "python3 scripts/missing-reused-release.py --self-test",
         "immutable Release reuse gate is not directly wired"),
        ("root-pages-source", ".github/workflows/build.yml", PAGES_SOURCE_CHECK,
         "python3 scripts/check-pages-manifest.py", "use only branch-backed Pages authority"),
        ("root-pre-upload-current", ".github/workflows/build.yml",
         "Revalidate current Release candidate immediately before signed artifact upload",
         "Trust signed artifact upload without revalidation",
         "required stage is missing"),
        ("root-pages", ".github/workflows/build.yml", LOCAL_PAGES_CHECK,
         "python3 scripts/missing-pages-byte-check.py", "required stage order drifted"),
        ("preview-pages", ".github/workflows/signed-pr-preview.yml", LOCAL_PAGES_CHECK,
         "python3 scripts/missing-pages-byte-check.py", "required stage is missing"),
        ("preview-pages-source", ".github/workflows/signed-pr-preview.yml", PAGES_SOURCE_CHECK,
         "python3 scripts/check-pages-manifest.py", "branch-backed Pages before key"),
        ("preview-independent-compare", ".github/workflows/signed-pr-preview.yml",
         "--compare-to _ci-independent", "--compare-to _ci-input",
         "default-owned rebuild"),
        ("preview-workflow-attestation", ".github/workflows/signed-pr-preview.yml",
         "--accept-workflow-attested-source", "--accept-unbound-workflow-source",
         "default-owned rebuild"),
        ("preview-trusted-dag", ".github/workflows/signed-pr-preview.yml",
         "needs: [validate, trusted-rebuild]", "needs: validate",
         "default-owned rebuild"),
        ("preview-trusted-artifact", ".github/workflows/signed-pr-preview.yml",
         "name: trusted-preview-rebuild-${{ needs.validate.outputs.head-sha }}",
         "name: firmware-independent-rebuild", "default-owned rebuild"),
        ("preview-stable-base", ".github/workflows/signed-pr-preview.yml",
         'if [ "$version" != "$EXPECTED_VERSION" ]; then',
         'if ! [[ "$version" =~ ^[0-9]+\\.[0-9]+\\.[0-9]+-PR-${PR_NUMBER}$ ]]; then',
         "default-owned rebuild"),
        ("preview-upload-order", ".github/workflows/signed-pr-preview.yml",
         "Revalidate PR immediately before signed artifact upload",
         "Revalidate PR after signed artifact upload", "required stage is missing"),
        ("preview-head-name", ".github/workflows/signed-pr-preview.yml",
         "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}-${{ needs.validate.outputs.head-sha }}",
         "name: tesla-key-esp32-pr${{ needs.validate.outputs.pr }}",
         "must bind the full current head SHA"),
        ("deploy-dag", ".github/workflows/build.yml", "    needs: [build, publish]\n",
         "    needs: build\n", "logic/build/rebuild/publish/deploy chain"),
        ("main-artifact-sha", ".github/workflows/build.yml",
         "name: tesla-key-esp32-${{ needs.build.outputs.display-version }}-${{ github.sha }}",
         "name: tesla-key-esp32-${{ needs.build.outputs.display-version }}",
         "exact SHA/version-bound artifact"),
        ("workflow-ccache-action", ".github/workflows/build.yml",
         "      - name: Compute release version\n",
         "      - name: Cache ccache\n"
         "        uses: actions/cache@0123456789abcdef0123456789abcdef01234567\n\n"
         "      - name: Compute release version\n",
         "must not restore or key a ccache artifact"),
        ("main-root-glob", ".github/workflows/build.yml",
         "            tesla-key-esp32.bin\n", "            tesla-key-esp32*.bin\n",
         "exact 12-file signed root"),
        ("preview-extra-root", ".github/workflows/signed-pr-preview.yml",
         "            tesla-key-esp32-c6.bin\n",
         "            tesla-key-esp32-c6.bin\n            tesla-key-esp32-surprise.bin\n",
         "exact 12-file signed root"),
        ("main-stage-input", ".github/workflows/build.yml",
         "            _fw/esp32/bootloader.bin\n", "",
         "exact 16 signer layout inputs"),
        ("deploy-release-byte-bind", ".github/workflows/build.yml",
         'python3 scripts/check-release-assets.py "$release_json" ./_deploy-input',
         "python3 scripts/missing-release-assets.py \"$release_json\" ./_deploy-input",
         "required stage is missing"),
        ("ci-chain", "scripts/ci-build-verify.sh",
         './scripts/test-release-contract.sh "$version" "$source_sha" _unsigned\n', "",
         "required stage is missing"),
        ("cjson-wiring", "scripts/ci-build-verify.sh",
         "CJSON_OOM_SANITIZE=1 bash ./test/run-cjson-oom-tests.sh",
         "true", "required stage is missing"),
        ("mqtt-json-wiring", "scripts/ci-build-verify.sh",
         "MQTT_JSON_SANITIZE=1 bash ./test/run-mqtt-json-publish-tests.sh",
         "true", "required stage is missing"),
        ("cjson-pin", "test/run-cjson-oom-tests.sh", '"v5.5.5"', '"v6.0.0"',
         "v5.5.5 cJSON"),
    )
    for name, relative, old, new, expected in mutations:
        with tempfile.TemporaryDirectory(prefix=f"build-gate-{name}-") as directory:
            fixture = Path(directory)
            copy_fixture(root, fixture)
            replace_once(fixture / relative, old, new)
            try:
                validate(fixture)
            except GateError as exc:
                require(expected in str(exc),
                        f"self-test {name} failed for the wrong reason: {exc}")
            else:
                raise GateError(f"self-test accepted mutation: {name}")

    loop_sites = (
        ("scripts/ci-build-all.sh", "build", "for target in $TARGETS; do"),
        ("scripts/ci-build-all.sh", "budget", "for target in $TARGETS; do"),
        ("scripts/ci-sign-artifacts.sh", "preflight", "for target in $TARGETS; do"),
        ("scripts/ci-sign-artifacts.sh", "sign", "for target in $TARGETS; do"),
        ("scripts/build-pages.sh", "pages", "for target in $TARGETS; do"),
        ("scripts/test-release-contract.sh", "verify-signed", EXACT_TARGET_LOOP),
        ("scripts/ci-build-verify.sh", "repro", EXACT_TARGET_LOOP),
    )
    skip_mutations = (
        ("continue", "  continue", "may not skip a target"),
        ("short-continue", '  [[ "$target" == esp32c6 ]] && continue',
         "may not skip a target"),
        ("target-if", '  if [[ "$target" == esp32c6 ]]; then :; fi',
         "may not branch on target"),
        ("target-case", '  case "$target" in esp32c6) : ;; *) : ;; esac',
         "may not case-dispatch on target"),
        ("ignored-failure", "  false || true", "may not ignore a target failure"),
        ("early-exit", "  exit 0", "may not exit successfully"),
        ("early-return", "  return 0", "may not exit successfully"),
    )
    for relative, loop_label, header in loop_sites:
        anchor = f"# EXACT_FOUR_TARGETS_BEGIN {loop_label}\n{header}"
        for mutation_name, injection, expected in skip_mutations:
            with tempfile.TemporaryDirectory(
                prefix=f"build-gate-loop-{loop_label}-{mutation_name}-"
            ) as directory:
                fixture = Path(directory)
                copy_fixture(root, fixture)
                replace_once(fixture / relative, anchor, f"{anchor}\n{injection}")
                try:
                    validate(fixture)
                except GateError as exc:
                    require(expected in str(exc),
                            f"self-test {relative}/{loop_label}/{mutation_name} "
                            f"failed for the wrong reason: {exc}")
                else:
                    raise GateError(
                        f"self-test accepted target-loop skip mutation: "
                        f"{relative}/{loop_label}/{mutation_name}"
                    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
        if args.self_test:
            self_test(args.root.resolve())
    except (GateError, OSError, SyntaxError, UnicodeError) as exc:
        print(f"build-gate-contract: {exc}", file=sys.stderr)
        return 1
    print("build-gate-contract: PASS (exact four-target build -> sign -> Pages -> repro"
          + (", mutation canaries" if args.self_test else "") + ")")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
