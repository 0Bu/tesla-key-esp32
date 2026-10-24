#!/usr/bin/env python3
"""Apply the canonical patch series only to exact reviewed source states."""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile

MARKER = ".tesla-key-patch-series"
SHA256 = re.compile(r"[0-9a-f]{64}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def clean_relative(name):
    require(isinstance(name, str) and name and "\\" not in name, "unsafe source path")
    path = PurePosixPath(name)
    require(not path.is_absolute() and all(p not in ("", ".", "..") for p in name.split("/")),
            "unsafe source path")
    return path


def real_directory(path):
    path = Path(os.path.abspath(path))
    for item in (*reversed(path.parents), path):
        info = item.lstat()
        require(stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode),
                "source directory is missing or symlinked")
    return path


def regular_leaf(root, relative):
    path = root / clean_relative(relative)
    real_directory(path.parent)
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and not stat.S_ISLNK(info.st_mode) and info.st_nlink == 1,
            "source or state leaf must be regular, not symlinked, and have exactly one hard link")
    return path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def series_marker(manifest):
    return "".join(f'{r["sha256"]} {r["name"]}\n' for r in manifest["patches"]).encode()


def validate_manifest(manifest):
    require(isinstance(manifest, dict) and set(manifest) == {"schema", "sourceCommit", "patches", "files"},
            "invalid patch-state manifest fields")
    require(type(manifest["schema"]) is int and manifest["schema"] == 1
            and isinstance(manifest["sourceCommit"], str)
            and re.fullmatch(r"[0-9a-f]{40}", manifest["sourceCommit"]), "invalid patch-state schema or commit")
    for collection, keys, identity in (("patches", {"name", "sha256"}, "name"),
                                       ("files", {"path", "beforeSha256", "afterSha256"}, "path")):
        rows = manifest[collection]
        require(isinstance(rows, list) and 0 < len(rows) <= 64, "invalid patch-state inventory")
        names = []
        for row in rows:
            require(isinstance(row, dict) and set(row) == keys, "invalid patch-state row")
            name = row[identity]
            clean_relative(name)
            if collection == "patches":
                require(re.fullmatch(r"[0-9]{4}-[a-z0-9-]+\.patch", name), "invalid patch filename")
            names.append(name)
            require(all(isinstance(row[k], str) and SHA256.fullmatch(row[k]) for k in keys - {identity}),
                    "invalid patch-state digest")
        require(names == sorted(set(names)), "duplicate or unordered patch-state inventory")


def apply_series(component, patch_dir, manifest, *, quiet=False):
    validate_manifest(manifest)
    component, patch_dir = real_directory(component), real_directory(patch_dir)
    require(sorted(p.name for p in patch_dir.iterdir()) == [r["name"] for r in manifest["patches"]],
            "patch filename inventory drifted")
    touched = set()
    for row in manifest["patches"]:
        path = regular_leaf(patch_dir, row["name"])
        require(digest(path) == row["sha256"], "patch digest drifted")
        text = path.read_text()
        before = re.findall(r"^--- a/(.+)$", text, re.M)
        after = re.findall(r"^\+\+\+ b/(.+)$", text, re.M)
        require(before and before == after, "unexpected patch source paths")
        for name in after:
            clean_relative(name)
            touched.add(name)
    require(touched == {r["path"] for r in manifest["files"]}, "patch touched-file inventory drifted")
    leaves = {r["path"]: regular_leaf(component, r["path"]) for r in manifest["files"]}
    observed = {name: digest(path) for name, path in leaves.items()}
    is_before = all(observed[r["path"]] == r["beforeSha256"] for r in manifest["files"])
    is_after = all(observed[r["path"]] == r["afterSha256"] for r in manifest["files"])
    expected_marker = series_marker(manifest)
    marker = component / MARKER
    if marker.exists() or marker.is_symlink():
        require(regular_leaf(component, MARKER).read_bytes() == expected_marker,
                "patch-state marker differs from the complete current series")
        require(is_after, "recorded patch postimage drifted; rematerialise the component")
        if not quiet:
            print("tesla-ble patch series: exact recorded postimages verified")
        return
    require(is_before or is_after, "unknown or mixed source state; rematerialise the component")
    if is_before and not is_after:
        for row in manifest["patches"]:
            subprocess.run(["patch", "-f", "-N", "-F0", "-V", "none", "-p1", "-d", str(component),
                            "-i", str(patch_dir / row["name"])], check=True,
                           stdout=subprocess.DEVNULL if quiet else None)
        require(all(digest(regular_leaf(component, r["path"])) == r["afterSha256"]
                    for r in manifest["files"]), "patched source differs from reviewed postimages")
    # mkstemp owns its unpredictable, exclusive regular file. replace never follows a marker link.
    fd, temporary = tempfile.mkstemp(prefix=MARKER + ".", dir=component)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(expected_marker)
        os.replace(temporary, marker)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    if not quiet:
        print("tesla-ble patch series: exact postimages verified and recorded")


def load_canonical(root):
    spec = importlib.util.spec_from_file_location("tesla_dependency_contract", root / "scripts/check-dependency-contract.py")
    dependency = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dependency)
    try:
        dependency.validate(root)
    except dependency.DependencyError as exc:
        raise ValueError(str(exc)) from exc
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, "duplicate patch-state JSON key")
            result[key] = value
        return result
    manifest = json.loads(regular_leaf(root, "scripts/tesla-ble-patch-state.json").read_text(), object_pairs_hook=unique)
    validate_manifest(manifest)
    require(manifest["sourceCommit"] == dependency.TESLA_RESOLVED_COMMIT
            and [(r["name"], r["sha256"]) for r in manifest["patches"]] == list(dependency.PATCH_INVENTORY),
            "patch-state manifest differs from the pinned dependency contract")
    return manifest, dependency


def configured_inputs(root, component, manifest, dependency):
    """Read-only exact postimage check and complete configure input list; no state adoption."""
    component = real_directory(component)
    require(regular_leaf(component, MARKER).read_bytes() == series_marker(manifest),
            "patch-state marker differs from the complete current series")
    for row in manifest["files"]:
        require(digest(regular_leaf(component, row["path"])) == row["afterSha256"],
                "recorded patch postimage drifted; rematerialise the component")
    names = {*dependency.FILE_DIGESTS, dependency.HARNESS_SCRIPT,
             "scripts/check-dependency-contract.py", "scripts/apply-tesla-ble-patches.sh",
             "scripts/apply-tesla-ble-patches.py"}
    paths = [regular_leaf(root, name) for name in sorted(names)]
    paths += [regular_leaf(root / "patches/tesla-ble", r["name"]) for r in manifest["patches"]]
    paths += [regular_leaf(component, r["path"]) for r in manifest["files"]]
    paths.append(component / MARKER)
    require(all(not any(c in str(path) for c in ("\n", "\r", ";")) for path in paths),
            "configure input paths cannot contain CMake list separators")
    return paths


def self_test():
    with tempfile.TemporaryDirectory(prefix="tesla-patch-state-") as temporary:
        root = Path(temporary).resolve()
        patches = root / "patches"
        patches.mkdir()
        contents = {"src/vehicle.cpp": (b"first\nalpha\nlast\n", b"first\nbeta\nlast\n"),
                    "include/client.h": (b"one\n", b"two\n")}
        patch_texts = ["--- a/src/vehicle.cpp\n+++ b/src/vehicle.cpp\n@@ -1,3 +1,3 @@\n first\n-alpha\n+beta\n last\n",
                       "--- a/include/client.h\n+++ b/include/client.h\n@@ -1 +1 @@\n-one\n+two\n"]
        patch_rows = []
        for index, text in enumerate(patch_texts):
            name = f"{index+1:04d}-fixture.patch"
            (patches / name).write_text(text)
            patch_rows.append({"name": name, "sha256": digest(patches / name)})
        manifest = {"schema": 1, "sourceCommit": "0" * 40, "patches": patch_rows,
                    "files": [{"path": name, "beforeSha256": hashlib.sha256(data[0]).hexdigest(),
                               "afterSha256": hashlib.sha256(data[1]).hexdigest()}
                              for name, data in sorted(contents.items())]}
        component = root / "component"
        def fresh(after=False):
            if component.exists():
                shutil.rmtree(component)
            component.mkdir()
            for name, data in contents.items():
                path = component / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data[int(after)])
        fresh()
        apply_series(component, patches, manifest)
        apply_series(component, patches, manifest)
        (component / MARKER).unlink()
        apply_series(component, patches, manifest)
        marker_bytes = (component / MARKER).read_bytes()
        controls = 0
        def reject(change, *, after=False, known_marker=False, contract=None):
            nonlocal controls
            fresh(after)
            if known_marker:
                (component / MARKER).write_bytes(marker_bytes)
            change()
            try:
                apply_series(component, patches, contract or manifest)
            except (ValueError, OSError, subprocess.CalledProcessError):
                controls += 1
                return
            raise AssertionError("unsafe patch source state accepted")
        reject(lambda: (component / "src/vehicle.cpp").write_text("first\nbeta\nlast\n"))
        reject(lambda: (component / "src/vehicle.cpp").write_text("drift\nalpha\nlast\n"))
        reject(lambda: (component / "src/vehicle.cpp").write_text("modified postimage\n"), after=True, known_marker=True)
        reject(lambda: None, known_marker=True)
        for bad in (b"", marker_bytes + marker_bytes, marker_bytes.splitlines()[0] + b"\n", b"malformed\n"):
            reject(lambda bad=bad: (component / MARKER).write_bytes(bad), after=True)
        reject(lambda: (component / "src/vehicle.cpp").unlink())
        def link_leaf():
            (component / "src/vehicle.cpp").unlink()
            (component / "src/vehicle.cpp").symlink_to(root / "outside")
        reject(link_leaf)
        def link_parent():
            shutil.rmtree(component / "src")
            (component / "src").symlink_to(root / "outside-dir", target_is_directory=True)
        reject(link_parent)
        reject(lambda: (component / MARKER).symlink_to(root / "outside"), after=True)
        reject(lambda: os.link(component / "src/vehicle.cpp", component / "second-link"))
        bad = copy.deepcopy(manifest)
        bad["files"][0]["path"] = "../outside"
        reject(lambda: None, contract=bad)
        bad = copy.deepcopy(manifest)
        bad["files"].append(bad["files"][0])
        reject(lambda: None, contract=bad)
        first = patches / patch_rows[0]["name"]
        original = first.read_bytes()
        reject(lambda: first.write_bytes(original + b"\n"))
        first.write_bytes(original)
        reject(lambda: (patches / "0003-extra.patch").write_text("extra"))
        (patches / "0003-extra.patch").unlink()
        reject(lambda: first.unlink())
        first.write_bytes(original)
        print(f"tesla-ble patch-state self-test: PASS (three exact-state positives, {controls} rejection controls)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, help="explicit native-harness component; canonical patches and hashes still required")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--self-test", action="store_true")
    mode.add_argument("--list-inputs", action="store_true", help="verify recorded postimages and print read-only CMake dependencies")
    args = parser.parse_args()
    require(not any(name.startswith("TESLA_BLE_") for name in os.environ),
            "caller TESLA_BLE_* environment overrides are forbidden")
    root = Path(__file__).resolve().parent.parent
    manifest, dependency = load_canonical(root)
    component = args.component or root / "managed_components/yoziru__tesla-ble"
    if args.self_test:
        require(args.component is None, "self-test does not accept an external component")
        self_test()
    elif args.list_inputs:
        for path in configured_inputs(root, component, manifest, dependency):
            print(path)
    else:
        apply_series(component, root / "patches/tesla-ble", manifest)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print(f"tesla-ble patch series: {exc}", file=sys.stderr)
        sys.exit(1)
