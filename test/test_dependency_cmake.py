#!/usr/bin/env python3
"""Real root CMake/source admission with inert Git inputs and passive consumer targets.

This exercises configure/build ordering, not ESP-IDF or a compiler. Run the Ninja mode in the
pinned IDF container as well as Unix Makefiles on the host before accepting a source change.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def copy_repository_inputs(source):
    spec = importlib.util.spec_from_file_location("dependency_fixture_contract",
                                                 ROOT / "scripts/check-dependency-contract.py")
    contract = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(contract)
    selected = {"CMakeLists.txt", "main/idf_component.yml", "esp-idf-toolchain.txt",
                "cmake/admit-dependency-sources.cmake", "scripts/dependency_source.py",
                "scripts/nanopb-source.json", "scripts/check-dependency-contract.py",
                "scripts/test-tesla-ble-harness.sh", "scripts/apply-tesla-ble-patches.sh",
                *contract.FILE_DIGESTS,
                *("dependencies.lock." + target for target in contract.TARGETS),
                *("patches/tesla-ble/" + name for name, _digest in contract.PATCH_INVENTORY)}
    for relative in selected:
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, target)


def run_generator(generator):
    with tempfile.TemporaryDirectory(prefix="dependency-cmake-tests-") as temporary:
        work = Path(temporary).resolve()
        origin = work / "origin"
        origin.mkdir()
        actual_git, cmake = shutil.which("git"), shutil.which("cmake")
        assert actual_git and cmake, "Git and CMake are required"
        if generator == "Ninja":
            assert shutil.which("ninja"), "Ninja mode requires Ninja"
        def run(command, **options):
            return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=40, **options)
        def git(*arguments):
            result = run([actual_git, "-c", "commit.gpgsign=false", "-c", "core.hooksPath=" + os.devnull,
                "-c", "maintenance.auto=false", "-c", "gc.auto=0", "-c", "user.name=Fixture",
                "-c", "user.email=fixture@example.invalid", "-C", str(origin), *arguments])
            assert result.returncode == 0, result.stderr.decode()
            return result.stdout.decode().strip()
        git("init", "--template=", "-q")
        names = ("pb.h", "pb_common.c", "pb_common.h", "pb_decode.c", "pb_decode.h",
                 "pb_encode.c", "pb_encode.h")
        for name in names:
            (origin / name).write_text("inert exact fixture " + name + "\n")
        (origin / "VERSION").write_text("0.4.9.1\n")
        (origin / "spm_headers/nanopb").mkdir(parents=True)
        for name in names:
            if name.endswith(".h"):
                (origin / "spm_headers/nanopb" / name).symlink_to("../../" + name)
        (origin / "CMakeLists.txt").write_text(
            'file(WRITE "${CMAKE_BINARY_DIR}/FORBIDDEN_DEPENDENCY_CONFIGURE" "unsafe")\n')
        git("add", ".")
        git("commit", "-qm", "Create owned inert nanopb source")
        commit = git("rev-parse", "HEAD")
        record = {"schema": 1, "version": "0.4.9.1", "commit": commit,
                  "repository": "https://github.com/nanopb/nanopb.git",
                  "files": {name: hashlib.sha256((origin / name).read_bytes()).hexdigest() for name in names}}
        source = work / "source"
        source.mkdir()
        copy_repository_inputs(source)
        (source / "version.txt").write_text("1.5.0\n")
        record_path = source / "scripts/nanopb-source.json"
        old_digest = hashlib.sha256(record_path.read_bytes()).hexdigest()
        record_path.write_text(json.dumps(record, indent=2) + "\n")
        new_digest = hashlib.sha256(record_path.read_bytes()).hexdigest()
        checker = source / "scripts/check-dependency-contract.py"
        text = checker.read_text()
        assert text.count(old_digest) == 1
        checker.write_text(text.replace(old_digest, new_digest))
        fake_idf = work / "idf"
        (fake_idf / "tools/cmake").mkdir(parents=True)
        (fake_idf / "tools/cmake/project.cmake").write_text(r'''
file(APPEND "${CMAKE_BINARY_DIR}/registered" "include\n")
function(idf_build_set_property)
endfunction()
function(idf_build_get_property output property)
    if(NOT property STREQUAL "BUILD_COMPONENTS")
        message(FATAL_ERROR "unexpected fixture property")
    endif()
    set(${output} "main;yoziru__tesla-ble;fixture_extra" PARENT_SCOPE)
endfunction()
function(idf_component_get_property output component property)
    if(NOT property STREQUAL "COMPONENT_LIB")
        message(FATAL_ERROR "unexpected fixture component property")
    endif()
    set(${output} "__idf_${component}" PARENT_SCOPE)
endfunction()
function(target_compile_definitions)
    if(NOT ARGV0 STREQUAL "__idf_yoziru__tesla-ble")
        message(FATAL_ERROR "unexpected fixture compile-policy target")
    endif()
endfunction()
function(execute_process)
    cmake_parse_arguments(P "" "RESULT_VARIABLE;OUTPUT_VARIABLE" "COMMAND" ${ARGV})
    list(GET P_COMMAND 0 executable)
    if(executable MATCHES "/apply-tesla-ble-patches.sh$")
        if(P_RESULT_VARIABLE)
            set(${P_RESULT_VARIABLE} 0 PARENT_SCOPE)
        endif()
        if(P_OUTPUT_VARIABLE)
            set(${P_OUTPUT_VARIABLE} "" PARENT_SCOPE)
        endif()
    else()
        _execute_process(${ARGV})
    endif()
endfunction()
macro(project)
    _project(${ARGV} NONE)
    include(FetchContent)
    FetchContent_Declare(nanopb DOWNLOAD_COMMAND "${CMAKE_COMMAND}" -E touch
        "${CMAKE_BINARY_DIR}/FORBIDDEN_LATE_TAG_POPULATE")
    FetchContent_GetProperties(nanopb)
    if(NOT nanopb_POPULATED)
        FetchContent_Populate(nanopb)
    endif()
    foreach(component IN ITEMS main yoziru__tesla-ble fixture_extra)
        add_custom_target("__idf_${component}" ALL COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/sentinel.py" "${CMAKE_BINARY_DIR}" "${component}")
    endforeach()
    add_custom_target("${CMAKE_PROJECT_NAME}.elf" ALL
        DEPENDS __idf_main __idf_yoziru__tesla-ble __idf_fixture_extra)
endmacro()
''')
        (source / "sentinel.py").write_text('''from pathlib import Path
import hashlib,sys
build=Path(sys.argv[1])
(build/('effect-'+sys.argv[2])).write_text(hashlib.sha256((build/'_deps/nanopb-src/pb.h').read_bytes()).hexdigest())
''')
        tools = work / "tools"
        tools.mkdir()
        wrapper = tools / "git"
        wrapper.write_text("#!" + sys.executable + "\nimport os,sys\nargs=sys.argv[1:]\n"
            "if 'fetch' in args:\n args=[arg.replace('https://github.com/nanopb/nanopb.git'," +
            repr(origin.as_uri()) + ") for arg in args]\n"
            "os.execv(" + repr(actual_git) + ",[" + repr(actual_git) + ",*args])\n")
        wrapper.chmod(0o700)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("GIT_", "SSH_", "PYTHON", "BASH_FUNC_"))
               and key not in ("GH_TOKEN", "GITHUB_TOKEN", "OTA_SIGNING_KEY", "OTA_SIGNING_KEY_FILE", "ENV", "BASH_ENV")}
        env.update(IDF_PATH=str(fake_idf), PATH=str(tools) + os.pathsep + os.environ["PATH"])
        build = work / "build"
        base = [cmake, "-S", str(source), "-B", str(build), "-G", generator,
                "-DPython3_EXECUTABLE=" + sys.executable]
        checks = []
        def configure(name, accepted=True):
            before = (build / "registered").read_bytes() if (build / "registered").exists() else b""
            result = run(base, env=env)
            assert (result.returncode == 0) == accepted, (name, result.stdout.decode(), result.stderr.decode())
            if not accepted:
                assert not (build / "registered").exists() or (build / "registered").read_bytes() == before
            assert not (build / "FORBIDDEN_LATE_TAG_POPULATE").exists()
            assert not (build / "FORBIDDEN_DEPENDENCY_CONFIGURE").exists()
            checks.append(name)
        def compile_effect(name, target, accepted):
            for path in build.glob("effect-*"):
                path.unlink()
            result = run([cmake, "--build", str(build), "--parallel", "4", "--target", target], env=env)
            assert (result.returncode == 0) == accepted, (name, result.stdout.decode(), result.stderr.decode())
            assert bool(list(build.glob("effect-*"))) == accepted, name
            checks.append(name)
        configure("cold-root-configure")
        configure("warm-root-configure")
        compile_effect("baseline-named-library", "__idf_main", True)
        leaf = build / "_deps/nanopb-src/pb.h"
        body, info = leaf.read_bytes(), leaf.stat()
        def mutate():
            leaf.write_bytes(b"X" + body[1:])
            os.utime(leaf, ns=(info.st_atime_ns, info.st_mtime_ns))
        def restore():
            leaf.write_bytes(body)
            os.utime(leaf, ns=(info.st_atime_ns, info.st_mtime_ns))
        mutate()
        for target in ("__idf_main", "__idf_yoziru__tesla-ble", "__idf_fixture_extra", "tesla-key-esp32.elf"):
            compile_effect("preserved-size-mtime-refusal-" + target, target, False)
        restore()
        compile_effect("restored-named-elf", "tesla-key-esp32.elf", True)
        owner = build / "_deps/.nanopb-src.tesla-source.json"
        held_owner = work / "held-owner"
        owner.rename(held_owner)
        compile_effect("missing-owner-refuses-before-effect", "__idf_main", False)
        held_owner.rename(owner)
        configure("restored-owner")
        location = leaf.parent
        held_source = work / "held-source"
        location.rename(held_source)
        compile_effect("missing-source-refuses-before-effect", "__idf_main", False)
        assert not location.exists()
        owner.rename(held_owner)
        compile_effect("missing-both-warm-state-never-cold-populates", "__idf_main", False)
        assert not location.exists() and not owner.exists()
        held_source.rename(location)
        held_owner.rename(owner)
        configure("restored-warm-state")
        root_cmake = source / "CMakeLists.txt"
        original = root_cmake.read_text()
        needle = "tesla_bind_dependency_admission()"
        assert original.count(needle) == 1
        root_cmake.write_text(original.replace(needle, "# single omitted build-admission delegation"))
        configure("valid-single-build-delegation-omission")
        mutate()
        compile_effect("effective-omission-unsafe-header-reaches-effect", "__idf_main", True)
        assert (build / "effect-main").read_text() != hashlib.sha256(body).hexdigest()
        restore()
        root_cmake.write_text(original)
        configure("restored-build-delegation")
        mutate()
        compile_effect("restored-delegation-refuses-same-mtime", "__idf_main", False)
        restore()
        print(json.dumps({"generator": generator, "checks": len(checks), "result": "PASS",
                          "limits": "Inert local Git, real root CMake/target graph and passive effects; no IDF/compiler/network/device/key proof."}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generator", choices=("Unix Makefiles", "Ninja"), default="Unix Makefiles")
    run_generator(parser.parse_args().generator)
