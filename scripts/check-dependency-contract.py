#!/usr/bin/env python3
"""Validate the exact pinned ESP-IDF/tesla-ble/Espressif-component dependency and patch contract."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
from collections.abc import Callable


TARGETS = ("esp32", "esp32s3", "esp32c3", "esp32c6")
TOOLCHAIN = (
    "v6.1@sha256:"
    "81893c71bb5e570088901f21def8684c25cd2a9020281bd01b843a7655edb18c\n"
)
IDF_VERSION = "6.1.0"
TESLA_GIT = "https://github.com/yoziru/tesla-ble.git"
TESLA_VERSION = "v5.2.0"
TESLA_RESOLVED_COMMIT = "07a4ef503a52f736009fdeba953f185aecc863f3"
TESLA_COMPONENT_HASH = "fb55938820781e8a731fc1557c0c4542bbf6833729062cfdb86a978b07016025"
LOCK_MANIFEST_HASH = "53e09d74068ad481d414bea246a46413be72e4638f19fa7733365f82bd057615"
# The host harness proves the PSA crypto port (patch 0006) against the Mbed TLS commit the firmware
# links (the host builds its builtin software drivers; the firmware sends AES-GCM through the ESP PSA
# driver). Its literal must equal this pin, and inside the pinned image (--idf-path) so must ESP-IDF's
# mbedtls submodule, so an IDF bump cannot leave that proof running on stale crypto.
MBEDTLS_COMMIT = "a2b32072ea898afc1ed5b6caf6931e36028c91d6"
HARNESS_SCRIPT = "scripts/test-tesla-ble-harness.sh"
IDF_MBEDTLS_SUBMODULE = "components/mbedtls/mbedtls"

# These digests deliberately cover comments, ordering and every transitive resolution. A reviewed
# dependency update changes this validator together with the lockfiles; a normal build may not
# silently rewrite an otherwise semantically plausible lock.
FILE_DIGESTS = {
    "esp-idf-toolchain.txt": "6ef2f1d9c15bb269430cbb4530d689022d914b12ac8610b539d4ac00d148d84f",
    "main/idf_component.yml": "3a67e0bf4d5896ab0a3336b14379e3208a3e9ca712e4b00dbc465b27015820ab",
    "dependencies.lock.esp32": "fb26215222e0d3330e72145588e7130996526b8f50008955c95ff18b8280fedc",
    "dependencies.lock.esp32s3": "e6f38bc6ceb30c8b5daa0013e3b00113beb2306e4eb9d52e5024ad2f91104a04",
    "dependencies.lock.esp32c3": "a125473e8928fb944eec32112d62b0a74be6a85d51cb8ba439ca6a6a48f244ac",
    "dependencies.lock.esp32c6": "d2178f0834e6091199c6b7595d4bce38f9a211a9edfbbe9541da1ed96d1bf549",
}
PATCH_INVENTORY = (
    (
        "0004-drop-unused-parental-controls-actions.patch",
        "6984321d34bdafe900244d0fe18052cc015a5b42ba0cf4e12e8ed9fb08791743",
    ),
    (
        "0005-align-session-counter-replay-with-signer-go.patch",
        "60fe7ee4533d89c160a7a757692bec51aba8b82f312d5352d9c720d2057f36b0",
    ),
    (
        "0006-port-crypto-bindings-to-psa.patch",
        "f5248e6529756e61ae3318aa2c8736a5ad28e68ec7e7a3a346dc3f3d970c41bd",
    ),
)
# Espressif components resolved from their GitHub release sources: (name, repository, path in the
# repository, manifest ref, resolved commit, component hash, IDF requirement, targets).
ESPRESSIF_COMPONENTS = (
    ("espressif/mdns", "https://github.com/espressif/esp-protocols.git", "components/mdns",
     "mdns-v1.13.1", "d61a590e07e0df4c66e7ac43feeceb0f1e0866f9",
     "424ec62b386bdfc522466053dab9b06ff389539bfa3da7632f32ef033874ef80", ">=5.0", TARGETS),
    ("espressif/w5500", "https://github.com/espressif/esp-eth-drivers.git", "w5500",
     "1f19456ec90b60424583a4138c075a78e1ab2297", "1f19456ec90b60424583a4138c075a78e1ab2297",
     "ef3e00d7f747f8a7b1792a3d3987122bb47dc33f948417b914e494986cb16322", ">=6.0", ("esp32s3",)),
)
# Espressif components that carry a Git submodule resolve from the component registry instead: a
# Git resolution hashes the submodule's `.git` gitlink, whose `gitdir:` path depends on where the
# Component Manager cache sits, so its lock hash verifies only on the machine that wrote it. The
# registry archive is fixed bytes. (name, resolved version, component hash, IDF requirement,
# targets); the manifest pins each as "==<version>".
REGISTRY_URL = "https://components.espressif.com/"
ESPRESSIF_REGISTRY_COMPONENTS = (
    ("espressif/cjson", "1.7.19~2",
     "e788323270d90738662d66fffa910bfe1fba019bba087f01557e70c40485b469", ">=5.0", TARGETS),
    ("espressif/mqtt", "1.1.0",
     "fb18bc3b65aa8c94693a9811ffc322cca8a65d92d5ec84983d3e385080e3969c", ">=5.3", TARGETS),
)
MANIFEST_LOGICAL_LINES = (
    "dependencies:",
    '  idf: ">=6.1,<7.0"',
    "  espressif/mdns:",
    '    git: "https://github.com/espressif/esp-protocols.git"',
    '    path: "components/mdns"',
    '    version: "mdns-v1.13.1"',
    "  espressif/cjson:",
    '    version: "==1.7.19~2"',
    "  espressif/mqtt:",
    '    version: "==1.1.0"',
    "  espressif/w5500:",
    '    git: "https://github.com/espressif/esp-eth-drivers.git"',
    '    path: "w5500"',
    '    version: "1f19456ec90b60424583a4138c075a78e1ab2297"',
    "    rules:",
    '      - if: "target == esp32s3"',
    "  yoziru/tesla-ble:",
    f'    git: "{TESLA_GIT}"',
    f'    version: "{TESLA_VERSION}"',
)
LOCK_TESLA_BLOCK = (
    "  yoziru/tesla-ble:\n"
    f"    component_hash: {TESLA_COMPONENT_HASH}\n"
    "    dependencies:\n"
    "    - name: idf\n"
    "      version: '>=5.0.1'\n"
    "    source:\n"
    f"      git: {TESLA_GIT}\n"
    "      path: .\n"
    "      type: git\n"
    "    targets:\n"
    "    - esp32\n"
    "    - esp32s3\n"
    "    - esp32c3\n"
    "    - esp32c6\n"
    f"    version: {TESLA_RESOLVED_COMMIT}\n"
)




def espressif_lock_block(name: str, git: str, path: str, commit: str, component_hash: str,
                         idf_requirement: str) -> str:
    return (
        f"  {name}:\n"
        f"    component_hash: {component_hash}\n"
        "    dependencies:\n"
        "    - name: idf\n"
        f"      version: '{idf_requirement}'\n"
        "    source:\n"
        f"      git: {git}\n"
        f"      path: {path}\n"
        "      type: git\n"
        f"    version: {commit}\n"
    )


def registry_lock_block(name: str, version: str, component_hash: str, idf_requirement: str) -> str:
    return (
        f"  {name}:\n"
        f"    component_hash: {component_hash}\n"
        "    dependencies:\n"
        "    - name: idf\n"
        "      require: private\n"
        f"      version: '{idf_requirement}'\n"
        "    source:\n"
        f"      registry_url: {REGISTRY_URL}\n"
        "      type: service\n"
        f"    version: {version}\n"
    )


def direct_dependencies(target: str) -> str:
    names = sorted(
        [name for name, *_, targets in ESPRESSIF_COMPONENTS if target in targets]
        + [name for name, *_, targets in ESPRESSIF_REGISTRY_COMPONENTS if target in targets]
        + ["idf", "yoziru/tesla-ble"]
    )
    return "direct_dependencies:\n" + "".join(f"- {name}\n" for name in names)


class DependencyError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise DependencyError(message)


def read_regular(path: Path) -> bytes:
    require(not path.is_symlink() and path.is_file(), f"missing/unsafe dependency input: {path}")
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        raise DependencyError(f"cannot stat dependency input {path}: {exc}") from exc
    require(stat.S_ISREG(mode), f"dependency input is not a regular file: {path}")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise DependencyError(f"cannot read dependency input {path}: {exc}") from exc


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def decode(data: bytes, label: str) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise DependencyError(f"{label} is not UTF-8 text") from exc


def logical_yaml_lines(text: str) -> tuple[str, ...]:
    return tuple(
        line.rstrip()
        for line in text.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )


def git_output(repository: Path, *args: str) -> str:
    try:
        completed = subprocess.run(
            ["git", "-c", f"safe.directory={repository}", "-C", str(repository), *args],
            check=True, capture_output=True, text=True, timeout=60,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise DependencyError(f"cannot read ESP-IDF Git metadata in {repository}: {exc}") from exc
    return completed.stdout


def check_idf_mbedtls(gitlink: str, submodule_head: str, expected: str = MBEDTLS_COMMIT) -> None:
    require(gitlink == f"160000 commit {expected}\t{IDF_MBEDTLS_SUBMODULE}\n",
            f"ESP-IDF {IDF_MBEDTLS_SUBMODULE} gitlink is not the harness Mbed TLS commit {expected}; "
            f"update MBEDTLS_REF in {HARNESS_SCRIPT} and rerun the V1 vectors")
    require(submodule_head == f"{expected}\n",
            f"ESP-IDF {IDF_MBEDTLS_SUBMODULE} checkout is not the harness Mbed TLS commit {expected}")


def validate_idf_mbedtls(idf_path: Path) -> None:
    require(idf_path.is_dir() and (idf_path / "tools/idf.py").is_file(),
            f"--idf-path is not an ESP-IDF checkout: {idf_path}")
    check_idf_mbedtls(
        git_output(idf_path, "ls-tree", "HEAD", "--", IDF_MBEDTLS_SUBMODULE),
        git_output(idf_path / IDF_MBEDTLS_SUBMODULE, "rev-parse", "HEAD"),
    )


def validate(root: Path) -> None:
    require(root.is_dir() and not root.is_symlink(), f"dependency root is missing/unsafe: {root}")

    toolchain_data = read_regular(root / "esp-idf-toolchain.txt")
    require(decode(toolchain_data, "esp-idf-toolchain.txt") == TOOLCHAIN,
            "esp-idf-toolchain.txt must pin exact ESP-IDF v6.1 and image digest")

    manifest_data = read_regular(root / "main/idf_component.yml")
    manifest_text = decode(manifest_data, "main/idf_component.yml")
    require(logical_yaml_lines(manifest_text) == MANIFEST_LOGICAL_LINES,
            "main/idf_component.yml exact IDF/Espressif-component/tesla-ble v5.2.0 Git contract drifted")

    actual_locks = tuple(sorted(path.name for path in root.glob("dependencies.lock.*")))
    expected_locks = tuple(f"dependencies.lock.{target}" for target in TARGETS)
    require(actual_locks == tuple(sorted(expected_locks)),
            f"target lockfile inventory drifted: expected {sorted(expected_locks)}, got {list(actual_locks)}")

    for target in TARGETS:
        relative = f"dependencies.lock.{target}"
        lock_data = read_regular(root / relative)
        lock_text = decode(lock_data, relative)
        require(lock_text.count(LOCK_TESLA_BLOCK) == 1,
                f"{relative}: tesla-ble resolved commit/component hash/Git/targets drifted")
        require(
            lock_text.count(f"  idf:\n    source:\n      type: idf\n    version: {IDF_VERSION}\n") == 1,
            f"{relative}: resolved ESP-IDF version must be exactly {IDF_VERSION}")
        for name, git, path, _ref, commit, component_hash, idf_requirement, targets in (
                ESPRESSIF_COMPONENTS):
            block = espressif_lock_block(name, git, path, commit, component_hash, idf_requirement)
            if target in targets:
                require(lock_text.count(block) == 1,
                        f"{relative}: {name} resolved commit/component hash/Git source drifted")
            else:
                require(f"  {name}:\n" not in lock_text,
                        f"{relative}: {name} must resolve only for {', '.join(targets)}")
        for name, version, component_hash, idf_requirement, targets in (
                ESPRESSIF_REGISTRY_COMPONENTS):
            block = registry_lock_block(name, version, component_hash, idf_requirement)
            if target in targets:
                require(lock_text.count(block) == 1,
                        f"{relative}: {name} resolved version/component hash/registry source drifted")
            else:
                require(f"  {name}:\n" not in lock_text,
                        f"{relative}: {name} must resolve only for {', '.join(targets)}")
        require(lock_text.count(f"target: {target}\n") == 1,
                f"{relative}: lock target must be exactly {target}")
        require(lock_text.count(f"manifest_hash: {LOCK_MANIFEST_HASH}\n") == 1,
                f"{relative}: manifest hash drifted")
        require(lock_text.count(direct_dependencies(target)) == 1,
                f"{relative}: direct dependency inventory/order drifted")

    patch_dir = root / "patches/tesla-ble"
    require(patch_dir.is_dir() and not patch_dir.is_symlink(),
            f"tesla-ble patch directory is missing/unsafe: {patch_dir}")
    entries = tuple(sorted(path.name for path in patch_dir.iterdir()))
    expected_patch_names = tuple(name for name, _ in PATCH_INVENTORY)
    require(entries == expected_patch_names,
            f"tesla-ble ordered patch filename inventory drifted: {entries}")
    for name, expected_digest in PATCH_INVENTORY:
        actual_digest = sha256(read_regular(patch_dir / name))
        require(actual_digest == expected_digest,
                f"tesla-ble patch digest drifted: {name}")

    for relative, expected_digest in FILE_DIGESTS.items():
        actual_digest = sha256(read_regular(root / relative))
        require(actual_digest == expected_digest,
                f"reviewed dependency file byte digest drifted: {relative}")

    harness_text = decode(read_regular(root / HARNESS_SCRIPT), HARNESS_SCRIPT)
    require(harness_text.count("MBEDTLS_REF=") == 1 and
            harness_text.count(f'\nMBEDTLS_REF="{MBEDTLS_COMMIT}"\n') == 1,
            f"{HARNESS_SCRIPT}: harness Mbed TLS commit must be exactly {MBEDTLS_COMMIT}")


def copy_fixture(root: Path, destination: Path) -> None:
    for relative in (*FILE_DIGESTS, HARNESS_SCRIPT):
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / relative, target)
    patch_dir = destination / "patches/tesla-ble"
    patch_dir.mkdir(parents=True, exist_ok=True)
    for name, _ in PATCH_INVENTORY:
        shutil.copy2(root / "patches/tesla-ble" / name, patch_dir / name)


def mutate_text(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    require(text.count(old) >= 1, f"self-test mutation source missing: {old}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def self_test(root: Path) -> None:
    validate(root)

    def text(relative: str, old: str, new: str) -> Callable[[Path], None]:
        return lambda fixture: mutate_text(fixture / relative, old, new)

    mutations: tuple[tuple[str, Callable[[Path], None], str], ...] = (
        ("toolchain-version", text("esp-idf-toolchain.txt", "v6.1", "v6.0"),
         "exact ESP-IDF v6.1"),
        ("toolchain-image", text("esp-idf-toolchain.txt", "81893", "91893"),
         "exact ESP-IDF v6.1"),
        ("manifest-version", text("main/idf_component.yml", 'version: "v5.2.0"',
                                  'version: "v5.1.0"'), "v5.2.0 Git contract"),
        ("manifest-git", text("main/idf_component.yml", TESLA_GIT,
                              "https://example.invalid/tesla-ble.git"), "v5.2.0 Git contract"),
        ("manifest-component-ref", text("main/idf_component.yml", '"mdns-v1.13.1"',
                                        '"mdns-v1.13.0"'), "Espressif-component"),
        ("manifest-w5500-rule", text("main/idf_component.yml", "target == esp32s3",
                                     "target != esp32s3"), "Espressif-component"),
        ("resolved-commit", text("dependencies.lock.esp32", TESLA_RESOLVED_COMMIT,
                                 "0" * 40), "resolved commit/component hash"),
        ("component-hash", text("dependencies.lock.esp32s3", TESLA_COMPONENT_HASH,
                                 "0" * 64), "resolved commit/component hash"),
        ("resolved-target", text("dependencies.lock.esp32c3", "    - esp32c6\n", ""),
         "resolved commit/component hash"),
        ("lock-target", text("dependencies.lock.esp32c6", "target: esp32c6",
                             "target: esp32c5"), "lock target must be exactly"),
        ("lock-idf", text("dependencies.lock.esp32s3", f"version: {IDF_VERSION}\n",
                          "version: 6.1.1\n"), "resolved ESP-IDF version"),
        ("component-commit", text("dependencies.lock.esp32c3",
                                  "d61a590e07e0df4c66e7ac43feeceb0f1e0866f9", "1" * 40),
         "espressif/mdns resolved commit"),
        ("component-source", text("dependencies.lock.esp32s3",
                                  "https://github.com/espressif/esp-eth-drivers.git",
                                  "https://example.invalid/esp-eth-drivers.git"),
         "espressif/w5500 resolved commit"),
        ("manifest-registry-version", text("main/idf_component.yml", '"==1.7.19~2"',
                                           '"^1.7.19"'), "Espressif-component"),
        ("registry-version", text("dependencies.lock.esp32", "version: 1.7.19~2\n",
                                  "version: 1.7.19~1\n"), "espressif/cjson resolved version"),
        ("registry-hash", text("dependencies.lock.esp32c6",
                               "fb18bc3b65aa8c94693a9811ffc322cca8a65d92d5ec84983d3e385080e3969c",
                               "0" * 64), "espressif/mqtt resolved version"),
        ("registry-to-git", text("dependencies.lock.esp32s3",
                                 f"      registry_url: {REGISTRY_URL}\n      type: service\n"
                                 "    version: 1.1.0\n",
                                 "      git: https://github.com/espressif/esp-mqtt.git\n"
                                 "      path: .\n      type: git\n"
                                 "    version: 1a1e5788a5cf57a0f44a3c6c061407f6c9be1026\n"),
         "espressif/mqtt resolved version"),
        ("w5500-other-target", text("dependencies.lock.esp32c6", "  idf:\n",
                                    "  espressif/w5500:\n    version: 1\n  idf:\n"),
         "espressif/w5500 must resolve only for esp32s3"),
        ("transitive-drift", text("dependencies.lock.esp32", "version: 3.0.0",
                                  "version: 2.0.0"), "file byte digest drifted"),
        ("patch-byte", text("patches/tesla-ble/0004-drop-unused-parental-controls-actions.patch",
                            "CarServer", "Carserver"), "patch digest drifted"),
        ("missing-lock", lambda fixture: (fixture / "dependencies.lock.esp32c6").unlink(),
         "lockfile inventory drifted"),
        ("extra-lock", lambda fixture: shutil.copy2(fixture / "dependencies.lock.esp32",
                                                    fixture / "dependencies.lock.esp32c5"),
         "lockfile inventory drifted"),
        ("renamed-patch", lambda fixture: (fixture / "patches/tesla-ble" / PATCH_INVENTORY[0][0]).rename(
            fixture / "patches/tesla-ble/0006-renamed.patch"), "patch filename inventory drifted"),
        ("missing-patch", lambda fixture: (fixture / "patches/tesla-ble" / PATCH_INVENTORY[1][0]).unlink(),
         "patch filename inventory drifted"),
        ("extra-patch", lambda fixture: shutil.copy2(
            fixture / "patches/tesla-ble" / PATCH_INVENTORY[0][0],
            fixture / "patches/tesla-ble/0006-extra.patch"), "patch filename inventory drifted"),
        ("harness-mbedtls", text(HARNESS_SCRIPT, MBEDTLS_COMMIT, "0" * 40),
         "harness Mbed TLS commit"),
        ("harness-mbedtls-override", text(HARNESS_SCRIPT, 'MBEDTLS_DIR="',
                                          'MBEDTLS_REF="main"\nMBEDTLS_DIR="'),
         "harness Mbed TLS commit"),
    )

    for name, mutate, expected in mutations:
        with tempfile.TemporaryDirectory(prefix=f"dependency-contract-{name}-") as directory:
            fixture = Path(directory)
            copy_fixture(root, fixture)
            mutate(fixture)
            try:
                validate(fixture)
            except DependencyError as exc:
                require(expected in str(exc),
                        f"self-test {name} failed for the wrong reason: {exc}")
            else:
                raise DependencyError(f"self-test accepted dependency mutation: {name}")

    good_gitlink = f"160000 commit {MBEDTLS_COMMIT}\t{IDF_MBEDTLS_SUBMODULE}\n"
    check_idf_mbedtls(good_gitlink, f"{MBEDTLS_COMMIT}\n")
    for name, gitlink, head in (
        ("idf-gitlink", good_gitlink.replace(MBEDTLS_COMMIT, "1" * 40), f"{MBEDTLS_COMMIT}\n"),
        ("idf-gitlink-missing", "", f"{MBEDTLS_COMMIT}\n"),
        ("idf-checkout", good_gitlink, f"{'2' * 40}\n"),
    ):
        try:
            check_idf_mbedtls(gitlink, head)
        except DependencyError as exc:
            require("harness Mbed TLS commit" in str(exc),
                    f"self-test {name} failed for the wrong reason: {exc}")
        else:
            raise DependencyError(f"self-test accepted ESP-IDF Mbed TLS drift: {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--idf-path", type=Path,
                        help="pinned ESP-IDF checkout whose mbedtls submodule must match the harness")
    args = parser.parse_args()
    root = args.root.absolute()
    try:
        validate(root)
        if args.idf_path is not None:
            validate_idf_mbedtls(args.idf_path.absolute())
        if args.self_test:
            self_test(root)
    except (DependencyError, OSError, UnicodeError) as exc:
        print(f"dependency-contract: {exc}", file=sys.stderr)
        return 1
    print(
        "dependency-contract: PASS "
        f"(ESP-IDF v6.1, {len(TARGETS)} locks, tesla-ble {TESLA_VERSION}, "
        f"{len(ESPRESSIF_COMPONENTS)} Espressif Git + "
        f"{len(ESPRESSIF_REGISTRY_COMPONENTS)} registry components, "
        f"{len(PATCH_INVENTORY)} patches"
        + (", image Mbed TLS = harness" if args.idf_path is not None else "")
        + (", mutation canaries" if args.self_test else "")
        + ")"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
