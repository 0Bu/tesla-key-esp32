#!/usr/bin/env python3
"""Exercise the shipped CMake admission block before incremental compiler invocations.

Only fixture digest constants change: synthetic source/patch bytes avoid network/vendor copies.
The actual checker functions, applier and CMake block execute unchanged in a disposable project.
"""
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fixture_constants(source, values):
    tree = ast.parse(source)
    lines = source.splitlines(keepends=True)
    replacements = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            name = node.targets[0].id
            if name in values:
                replacements.append((node.lineno - 1, node.end_lineno, f"{name} = {values[name]!r}\n"))
    assert len(replacements) == len(values)
    for first, last, text in sorted(replacements, reverse=True):
        lines[first:last] = [text]
    result = "".join(lines)
    functions = lambda text: [ast.dump(n) for n in ast.parse(text).body
                              if isinstance(n, (ast.FunctionDef, ast.ClassDef))]
    assert functions(source) == functions(result), "fixture changed validator behavior"
    return result


def make_fixture(destination):
    spec = importlib.util.spec_from_file_location("dependency", ROOT / "scripts/check-dependency-contract.py")
    dependency = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dependency)
    dependency.copy_fixture(ROOT, destination)
    for name in ("scripts/check-dependency-contract.py", "scripts/apply-tesla-ble-patches.sh",
                 "scripts/apply-tesla-ble-patches.py"):
        shutil.copy2(ROOT / name, destination / name)
    manifest = json.loads((ROOT / "scripts/tesla-ble-patch-state.json").read_text())
    component = destination / "managed_components/yoziru__tesla-ble"
    for row in manifest["files"]:
        leaf = component / row["path"]
        leaf.parent.mkdir(parents=True, exist_ok=True)
        leaf.write_bytes(b"before\n")
        row["beforeSha256"], row["afterSha256"] = sha(b"before\n"), sha(b"after\n")
    for index, row in enumerate(manifest["patches"]):
        paths = [r["path"] for r in manifest["files"][index::len(manifest["patches"])]]
        raw = "".join(f"--- a/{p}\n+++ b/{p}\n@@ -1 +1 @@\n-before\n+after\n" for p in paths).encode()
        (destination / "patches/tesla-ble" / row["name"]).write_bytes(raw)
        row["sha256"] = sha(raw)
    raw_manifest = (json.dumps(manifest, indent=2) + "\n").encode()
    (destination / "scripts/tesla-ble-patch-state.json").write_bytes(raw_manifest)
    digests = dict(dependency.FILE_DIGESTS)
    digests["scripts/tesla-ble-patch-state.json"] = sha(raw_manifest)
    validator = destination / "scripts/check-dependency-contract.py"
    validator.write_text(fixture_constants(validator.read_text(), {
        "FILE_DIGESTS": digests,
        "PATCH_INVENTORY": tuple((r["name"], r["sha256"]) for r in manifest["patches"]),
    }))
    source = (ROOT / "CMakeLists.txt").read_text()
    first, last = "# TESLA_PATCH_STATE_BEGIN", "# TESLA_PATCH_STATE_END"
    assert source.count(first) == source.count(last) == 1
    block = source[source.index(first):source.index(last) + len(last)]
    log_first = "idf_component_get_property(TESLA_BLE_COMPONENT_LIB yoziru__tesla-ble COMPONENT_LIB)"
    log_last = "target_compile_definitions(${TESLA_BLE_COMPONENT_LIB} PRIVATE TESLA_BLE_LOG_LEVEL=2)"
    assert source.count(log_first) == source.count(log_last) == 1
    log_policy = source[source.index(log_first):source.index(log_last) + len(log_last)]
    (destination / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.16)\nproject(patch_incremental_fixture C)\n"
        "add_executable(probe probe.c)\n"
        "function(idf_component_get_property output component property)\n"
        "  if(NOT component STREQUAL \"yoziru__tesla-ble\" OR NOT property STREQUAL \"COMPONENT_LIB\")\n"
        "    message(FATAL_ERROR \"unexpected component property\")\n"
        "  endif()\n"
        "  set(${output} probe PARENT_SCOPE)\n"
        "endfunction()\n"
        + log_policy + "\n" + block + "\n")
    (destination / "probe.c").write_text("int main(void){return 0;}\n")


def main():
    cmake, compiler = shutil.which("cmake"), shutil.which("cc") or shutil.which("gcc")
    assert cmake and compiler, "CMake and a C compiler are required"
    generator = "Ninja" if shutil.which("ninja") else "Unix Makefiles"
    with tempfile.TemporaryDirectory(prefix="tesla-patch-incremental-") as temporary:
        root = Path(temporary).resolve()
        base = root / "base"
        base.mkdir()
        make_fixture(base)
        component_path = Path("managed_components/yoziru__tesla-ble")
        marker_path = component_path / ".tesla-key-patch-series"
        leaf_path = component_path / "include/client.h"
        patch_path = Path("patches/tesla-ble/0004-drop-unused-parental-controls-actions.patch")
        def append(path, value):
            with path.open("a") as out:
                out.write(value)
        cases = (
            ("no-op", lambda d: None, True, False, None),
            ("helper-only", lambda d: append(d / "scripts/apply-tesla-ble-patches.py", "\n# fixture change\n"), True, True, "exact recorded postimages verified"),
            ("marker-drift", lambda d: append(d / marker_path, "invalid\n"), False, True, "marker differs"),
            ("postimage-drift", lambda d: append(d / leaf_path, "unknown\n"), False, True, "postimage drifted"),
            ("patch-drift", lambda d: append(d / patch_path, "\n"), False, True, "patch digest drifted"),
            ("manifest-drift", lambda d: append(d / "scripts/tesla-ble-patch-state.json", "\n"), False, True, "dependency file byte digest drifted"),
            ("extra-patch", lambda d: (d / "patches/tesla-ble/9999-extra.patch").write_text("extra"), False, True, "patch filename inventory drifted"),
            ("extra-patch-directory", lambda d: (d / "patches/tesla-ble/extra").mkdir(), False, True, "patch filename inventory drifted"),
            ("extra-lock", lambda d: (d / "dependencies.lock.foreign").write_text("extra"), False, True, "lockfile inventory drifted"),
            ("missing-leaf", lambda d: (d / leaf_path).unlink(), False, True, "client.h"),
            ("missing-marker-known-postimages", lambda d: (d / marker_path).unlink(), True, True, "exact postimages verified and recorded"),
            ("missing-marker-unknown-source", lambda d: ((d / marker_path).unlink(), append(d / leaf_path, "unknown\n")), False, True, "unknown or mixed source state"),
        )
        for label, change, accepted, dirty_probe, needle in cases:
            project = root / (label + " with spaces")
            shutil.copytree(base, project)
            marker = project / "compiler-started"
            wrapper = project / "compiler-wrapper.py"
            wrapper.write_text(
                "#!/usr/bin/env python3\nimport os,sys\nfrom pathlib import Path\n"
                f"with Path({str(marker)!r}).open('a') as f:f.write('compiler started\\n')\n"
                f"os.execv({compiler!r},[{compiler!r},*sys.argv[1:]])\n")
            wrapper.chmod(0o755)
            def run(arguments):
                return subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, timeout=30)
            configure = run([cmake, "-S", str(project), "-B", str(project / "build"), "-G", generator,
                             "-DCMAKE_C_COMPILER=" + str(wrapper)])
            assert configure.returncode == 0, (label, configure.stdout)
            initial = run([cmake, "--build", str(project / "build")])
            assert initial.returncode == 0 and marker.exists(), (label, initial.stdout)
            marker.unlink()
            change(project)
            if dirty_probe:
                # Delete the owned object instead of relying on filesystem timestamp resolution:
                # if admission is bypassed, this guarantees an actual compiler invocation.
                objects = list((project / "build").rglob("probe.c.o"))
                assert len(objects) == 1, objects
                objects[0].unlink()
            rebuilt = run([cmake, "--build", str(project / "build")])
            assert (rebuilt.returncode == 0) == accepted, (label, rebuilt.stdout)
            assert marker.exists() == (accepted and dirty_probe), (label, rebuilt.stdout)
            if needle:
                assert needle in rebuilt.stdout, (label, rebuilt.stdout)
            if not dirty_probe:
                assert "exact recorded postimages verified" not in rebuilt.stdout, (label, rebuilt.stdout)
            print(f"PASS {label}: rebuild rc={rebuilt.returncode}, compiler started={marker.exists()}")
    print(f"patch incremental admission: PASS ({len(cases)} actual {generator} controls)")


if __name__ == "__main__":
    main()
