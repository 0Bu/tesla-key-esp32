#!/usr/bin/env python3
"""Keep docs/ESP_IDF_MATRIX.md bound to the actual ESP-IDF surface without network access.

The matrix copies facts that change with ordinary work: CMake components, managed dependencies,
every active sdkconfig.defaults assignment and the non-project headers included under main/. This
checker re-derives each of them from the tree and fails on any difference in either direction, so
a change that moves the ESP-IDF surface cannot merge with a stale matrix. It checks the version pin
and shape of ESP-IDF documentation and source links, not their remote availability, and it does
not judge whether a decision text is still right.

Exit: 0 = clean, 1 = drift findings, 2 = malformed or missing audit input, or a checker fault.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
from typing import Callable

DOC = Path("docs/ESP_IDF_MATRIX.md")
TARGETS = ["esp32", "esp32s3", "esp32c3", "esp32c6"]
USED_STATUSES = ("✅ Direct", "🧩 Backend", "📦 Managed")
EVALUATED_STATUSES = ("🔬 Evaluate", "⏸ Conditional", "🚫 Not planned", "⛔ Unsupported")
# Evaluated decisions that guard a deliberate boundary; deleting one must be a reviewed choice.
REQUIRED_EVALUATED = ("ULP", "esp_https_ota")
TABLE_HEADERS = {
    "used": ["ID", "Feature", "Status", "Targets", "Component/source", "Project use", "Evidence",
             "Official documentation"],
    "evaluated": ["ID", "Feature", "Status", "Targets", "Decision", "Checked against",
                  "Official documentation"],
    "headers": ["Headers", "Feature ID"],
    "sdkconfig": ["Setting", "Value", "File", "Feature ID"],
}
# Headers that need no matrix row: the C standard library and newlib's sys/ headers. C++ standard
# headers have neither an extension nor a directory (<array>, <cstdint>). Everything else that is
# not a project header must be listed, so an unfamiliar ESP-IDF header cannot slip past a prefix.
C_STANDARD = {
    "alloca.h", "assert.h", "ctype.h", "errno.h", "float.h", "inttypes.h", "limits.h", "locale.h",
    "math.h", "setjmp.h", "signal.h", "stdarg.h", "stdatomic.h", "stdbool.h", "stddef.h",
    "stdint.h", "stdio.h", "stdlib.h", "string.h", "time.h", "wchar.h",
}
REPAIR_VARIABLE = "${TESLA_REPAIR_REQUIRES}"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp"}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
BLOCK_COMMENT = re.compile(r"^\s*/\*.*?\*/", re.MULTILINE | re.DOTALL)
TICKED = re.compile(r"`([^`]+)`")
DOCS_LINK = re.compile(r"https://docs\.espressif\.com/projects/esp-idf/en/([^/]+)/([^/]+)/")
SOURCE_LINK = re.compile(r"https://github\.com/espressif/esp-idf/(?:blob|tree)/([^/]+)/")
ASSIGNMENT = re.compile(r"^(CONFIG_[A-Z0-9_]+)=(.*)$")
NOT_SET = re.compile(r"^# (CONFIG_[A-Z0-9_]+) is not set$")


class MatrixError(RuntimeError):
    """Malformed or missing audit input; distinct from drift findings."""


def read(root: Path, relative: str | Path) -> str:
    try:
        return (root / relative).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise MatrixError(f"{relative}: unreadable: {exc}") from exc


def block(text: str, name: str) -> str:
    start, end = f"<!-- esp-idf-matrix:{name}:start -->", f"<!-- esp-idf-matrix:{name}:end -->"
    if text.count(start) != 1 or text.count(end) != 1 or text.index(start) > text.index(end):
        raise MatrixError(f"{DOC}: needs exactly one {name} marker block")
    return text[text.index(start) + len(start):text.index(end)]


def rows(text: str, name: str) -> list[list[str]]:
    parsed: list[list[str]] = []
    for line in block(text, name).splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if all(cell and set(cell) <= {"-", ":"} for cell in cells):
            continue
        parsed.append(cells)
    if not parsed or parsed[0] != TABLE_HEADERS[name]:
        raise MatrixError(f"{DOC}: {name} table must start with columns {TABLE_HEADERS[name]}")
    for cells in parsed[1:]:
        if len(cells) != len(TABLE_HEADERS[name]):
            raise MatrixError(f"{DOC}: {name} row has {len(cells)} cells: {' | '.join(cells)[:80]}")
    if len(parsed) < 2:
        raise MatrixError(f"{DOC}: {name} table is empty")
    return parsed[1:]


def lock_idf_version(text: str) -> str | None:
    match = re.search(r"^  idf:\n(?:    .*\n)*?    version: (\S+)", text, re.MULTILINE)
    return match.group(1) if match else None


def manifest(root: Path) -> tuple[str | None, list[str]]:
    floor, keys, inside = None, [], False
    for line in read(root, "main/idf_component.yml").splitlines():
        if re.match(r"^\S", line):
            inside = line.startswith("dependencies:")
            continue
        match = re.match(r'^  (["\']?)([\w./-]+)\1:\s*(.*)$', line) if inside else None
        if not match:
            continue
        if match.group(2) == "idf":
            floor = match.group(3).strip().strip("\"'")
        else:
            keys.append(match.group(2))
    return floor, keys


def cmake_components(root: Path) -> tuple[list[str], list[str]]:
    text = read(root, "main/CMakeLists.txt")
    findings: list[str] = []
    match = re.search(r"idf_component_register\((.*?)\n\)", text, re.DOTALL)
    if not match or "REQUIRES" not in match.group(1):
        raise MatrixError("main/CMakeLists.txt: idf_component_register REQUIRES not found")
    names = match.group(1).split("REQUIRES", 1)[1].split()
    for variable in [name for name in names if name.startswith("${")]:
        if variable != REPAIR_VARIABLE:
            findings.append(f"unexpected CMake variable {variable} in REQUIRES")
    appended = re.findall(r"list\(APPEND TESLA_REPAIR_REQUIRES ([^)]*)\)", text)
    if REPAIR_VARIABLE in names and not appended:
        findings.append(f"{REPAIR_VARIABLE} is required but no list(APPEND ...) defines it")
    names = [name for name in names if not name.startswith("${")] + " ".join(appended).split()
    return sorted(set(names)), findings


def defaults_files(root: Path) -> list[tuple[str, Path]]:
    found: list[tuple[str, Path]] = []
    for prefix, label in ((Path("."), "base"), (Path("maintenance/config-repair"), "repair")):
        base = prefix / "sdkconfig.defaults"
        if not (root / base).is_file():
            raise MatrixError(f"{base}: missing")
        found.append((label, base))
        overlays = sorted(path.name for path in (root / prefix).glob("sdkconfig.defaults.*"))
        unknown = [name for name in overlays if name.split(".", 2)[2] not in TARGETS]
        if unknown:
            raise MatrixError(f"{prefix}: unclassified defaults overlay {', '.join(unknown)}")
        for target in TARGETS:
            path = prefix / f"sdkconfig.defaults.{target}"
            if (root / path).is_file():
                found.append((target if label == "base" else f"repair.{target}", path))
    return found


def active_assignments(root: Path) -> list[tuple[str, str, str]]:
    result: list[tuple[str, str, str]] = []
    for label, path in defaults_files(root):
        for line in read(root, path).splitlines():
            if match := ASSIGNMENT.match(line):
                result.append((match.group(1), match.group(2), label))
            elif match := NOT_SET.match(line):
                result.append((match.group(1), "is not set", label))
    return result


def standard_header(header: str) -> bool:
    if "." not in header and "/" not in header:
        return True  # C++ standard library: <array>, <cstdint>
    return header in C_STANDARD or header.startswith("sys/")


def included_headers(root: Path) -> set[str]:
    main = root / "main"
    found: set[str] = set()
    for path in sorted(main.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        text = BLOCK_COMMENT.sub("", read(root, path.relative_to(root)))
        for header in INCLUDE.findall(text):
            # A project header can start with an IDF-looking prefix (nvs_storage.hpp).
            if any((base / header).exists() for base in (main, main / "logic", path.parent)):
                continue
            if not standard_header(header):
                found.add(header)
    return found


def validate(root: Path) -> tuple[list[str], dict[str, int]]:
    text = read(root, DOC)
    findings: list[str] = []
    meta_match = re.search(r"<!-- esp-idf-matrix:metadata (\{.*?\}) -->", text)
    if not meta_match:
        raise MatrixError(f"{DOC}: metadata comment missing")
    try:
        meta = json.loads(meta_match.group(1))
    except json.JSONDecodeError as exc:
        raise MatrixError(f"{DOC}: metadata is not JSON: {exc}") from exc
    version = str(meta.get("version", ""))
    if meta.get("targets") != TARGETS:
        findings.append(f"metadata targets {meta.get('targets')} differ from {TARGETS}")
    for target in TARGETS:
        if not (root / f"sdkconfig.defaults.{target}").is_file():
            findings.append(f"target {target} has no sdkconfig.defaults.{target}")
        locked = lock_idf_version(read(root, f"dependencies.lock.{target}"))
        if locked != version:
            findings.append(f"dependencies.lock.{target} resolves ESP-IDF {locked}, matrix says {version}")
    floor, managed = manifest(root)
    if floor != meta.get("idf_floor"):
        findings.append(f"main/idf_component.yml idf floor {floor!r}, matrix says {meta.get('idf_floor')!r}")
    channel = "v" + ".".join(version.split(".")[:2])
    if not read(root, "esp-idf-toolchain.txt").startswith(channel + "@"):
        findings.append(f"esp-idf-toolchain.txt does not pin {channel}")
    for link_version, link_target in DOCS_LINK.findall(text):
        if link_version != channel:
            findings.append(f"documentation link uses {link_version}, pinned channel is {channel}")
        if link_target not in TARGETS:
            findings.append(f"documentation link names unsupported target {link_target}")
    for ref in SOURCE_LINK.findall(text):
        if ref != channel:
            findings.append(f"ESP-IDF source link uses {ref}, pinned channel is {channel}")

    used = rows(text, "used")
    evaluated = rows(text, "evaluated")
    ids = [row[0] for row in used + evaluated]
    for duplicate in sorted({value for value in ids if ids.count(value) > 1}):
        findings.append(f"duplicate feature id {duplicate}")
    used_ids = {row[0] for row in used}
    for table, prefix, allowed, other in (
            (used, "U", USED_STATUSES, EVALUATED_STATUSES),
            (evaluated, "N", EVALUATED_STATUSES, USED_STATUSES)):
        for row in table:
            if not re.fullmatch(prefix + r"\d{2}", row[0]):
                findings.append(f"row id {row[0]} must match {prefix}NN")
            if not any(status in row[2] for status in allowed) or any(status in row[2] for status in other):
                findings.append(f"{row[0]} has an invalid status: {row[2]}")
            if "https://" not in row[-1]:
                findings.append(f"{row[0]} has no official documentation link")
            for token in re.findall(r"esp32\w*", row[3]):
                if token not in TARGETS:
                    findings.append(f"{row[0]} names unsupported target {token}")
    for required in REQUIRED_EVALUATED:
        if not any(required in row[1] for row in evaluated):
            findings.append(f"required evaluated decision about {required} is missing")

    components, cmake_findings = cmake_components(root)
    findings += cmake_findings
    current = set(components) | set(managed)
    claimed: dict[str, str] = {}
    for row in used:
        for name in TICKED.findall(row[4]):
            claimed[name] = row[0]
    for name in sorted(current - claimed.keys()):
        findings.append(f"component {name} has no used-table row")
    for name in sorted(claimed.keys() - current):
        findings.append(f"{claimed[name]} names {name}, which is not a current component")

    documented = []
    for setting, value, label, feature in rows(text, "sdkconfig"):
        documented.append((setting.strip("`"), value.strip("`"), label))
        if feature not in used_ids:
            findings.append(f"{setting} maps to missing used row {feature}")
    actual = active_assignments(root)
    if documented != actual:
        for item in [entry for entry in actual if entry not in documented]:
            findings.append(f"sdkconfig assignment missing from matrix: {item[0]}={item[1]} ({item[2]})")
        for item in [entry for entry in documented if entry not in actual]:
            findings.append(f"stale sdkconfig row in matrix: {item[0]}={item[1]} ({item[2]})")
        if sorted(documented) == sorted(actual):
            findings.append("sdkconfig rows are not in file order")

    listed: dict[str, str] = {}
    for headers, feature in rows(text, "headers"):
        if feature not in used_ids:
            findings.append(f"header row maps to missing used row {feature}")
        for header in TICKED.findall(headers):
            listed[header] = feature
    included = included_headers(root)
    for header in sorted(included - listed.keys()):
        findings.append(f"unlisted header {header} included under main/")
    for header in sorted(listed.keys() - included):
        findings.append(f"stale header {header} is no longer included under main/")

    counts = {"used": len(used), "evaluated": len(evaluated), "components": len(components),
              "managed": len(managed), "config": len(actual), "headers": len(included)}
    return findings, counts


COPY_GLOBS = ("docs/ESP_IDF_MATRIX.md", "main/CMakeLists.txt", "main/idf_component.yml",
              "esp-idf-toolchain.txt", "dependencies.lock.*", "sdkconfig.defaults*",
              "maintenance/config-repair/sdkconfig.defaults*")


def fixture(source: Path, destination: Path) -> None:
    paths = [path for pattern in COPY_GLOBS for path in source.glob(pattern)]
    paths += [path for path in (source / "main").rglob("*")
              if path.is_file() and path.suffix in SOURCE_SUFFIXES]
    for path in paths:
        target = destination / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)


def edit(root: Path, relative: str, old: str, new: str) -> None:
    path = root / relative
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise MatrixError(f"self-test anchor missing in {relative}: {old[:60]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def append(root: Path, relative: str, line: str) -> None:
    with (root / relative).open("a", encoding="utf-8") as handle:
        handle.write(line)


def strip_includes(root: Path, header: str) -> None:
    pattern = re.compile(r'^\s*#\s*include\s*[<"]' + re.escape(header) + r'[>"].*\n', re.MULTILINE)
    hits = 0
    for path in (root / "main").rglob("*"):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            text = path.read_text(encoding="utf-8")
            new, count = pattern.subn("", text)
            hits += count
            path.write_text(new, encoding="utf-8")
    if not hits:
        raise MatrixError(f"self-test anchor missing: no include of {header}")


def swap_first_assignments(root: Path) -> None:
    path = root / "sdkconfig.defaults"
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    positions = [index for index, line in enumerate(lines) if ASSIGNMENT.match(line.rstrip("\n"))][:2]
    if len(positions) < 2:
        raise MatrixError("self-test anchor missing: two assignments in sdkconfig.defaults")
    first, second = positions
    lines[first], lines[second] = lines[second], lines[first]
    path.write_text("".join(lines), encoding="utf-8")


Mutation = Callable[[Path], None]


def mutations(root: Path) -> list[tuple[str | None, Mutation]]:
    """Each entry reintroduces one drift class; None means the edit must stay clean."""
    doc = str(DOC)
    used = rows(read(root, DOC), "used")
    first, second = used[0], used[1]
    first_status = f"| {first[0]} | {first[1]} | {first[2]} |"
    return [
        ("metadata targets", lambda r: edit(r, doc, '"targets":["esp32",', '"targets":["esp32h2","esp32",')),
        ("resolves ESP-IDF 6.1.1", lambda r: edit(r, "dependencies.lock.esp32c6", "    version: 6.1.0", "    version: 6.1.1")),
        ("idf floor", lambda r: edit(r, "main/idf_component.yml", 'idf: ">=6.1,<7.0"', 'idf: ">=6.0,<7.0"')),
        ("does not pin v6.1", lambda r: edit(r, "esp-idf-toolchain.txt", "v6.1@", "v6.2@")),
        ("documentation link uses v6.0", lambda r: edit(r, doc, "/en/v6.1/esp32/", "/en/v6.0/esp32/")),
        ("unsupported target esp32h2", lambda r: edit(r, doc, "/en/v6.1/esp32s3/", "/en/v6.1/esp32h2/")),
        ("source link uses v6.0", lambda r: edit(r, doc, "esp-idf/blob/v6.1/", "esp-idf/blob/v6.0/")),
        (f"duplicate feature id {first[0]}", lambda r: edit(r, doc, f"| {second[0]} |", f"| {first[0]} |")),
        (f"{first[0]} has an invalid status", lambda r: edit(r, doc, first_status, first_status.rsplit("|", 2)[0] + "| 🚫 Not planned |")),
        ("required evaluated decision about ULP", lambda r: edit(r, doc, "ULP-FSM, ULP-RISC-V and LP core", "Low-power coprocessors")),
        ("must start with columns", lambda r: edit(r, doc, "| Setting | Value | File | Feature ID |", "| Setting | Value | Feature ID | File |")),
        ("component esp_adc has no used-table row", lambda r: edit(r, "main/CMakeLists.txt", "    REQUIRES\n", "    REQUIRES\n        esp_adc\n")),
        ("names esp_eth, which is not a current component", lambda r: edit(r, "main/CMakeLists.txt", "        esp_eth\n", "")),
        ("names esp_driver_uart, which is not a current component", lambda r: edit(r, "main/CMakeLists.txt", "list(APPEND TESLA_REPAIR_REQUIRES esp_driver_uart)", "")),
        ("unexpected CMake variable ${EXTRA}", lambda r: edit(r, "main/CMakeLists.txt", "    REQUIRES\n", "    REQUIRES\n        ${EXTRA}\n")),
        ("component espressif/led_strip has no used-table row", lambda r: edit(r, "main/idf_component.yml", "dependencies:\n", "dependencies:\n  espressif/led_strip:\n    version: \"==3.0.0\"\n")),
        ("component espressif/esp_lcd_st7735 has no used-table row", lambda r: edit(r, "main/idf_component.yml", "dependencies:\n", "dependencies:\n  \"espressif/esp_lcd_st7735\":\n    version: \"==1.0.0\"\n")),
        ("sdkconfig assignment missing", lambda r: append(r, "sdkconfig.defaults", "CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n\n")),
        ("stale sdkconfig row", lambda r: edit(r, "sdkconfig.defaults.esp32", "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6", "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=7")),
        ("not in file order", swap_first_assignments),
        ("unclassified defaults overlay", lambda r: (r / "sdkconfig.defaults.esp32h2").write_text("CONFIG_X=y\n")),
        ("maps to missing used row U99", lambda r: edit(r, doc, "| `CONFIG_APP_REPRODUCIBLE_BUILD` | `y` | base | U01 |", "| `CONFIG_APP_REPRODUCIBLE_BUILD` | `y` | base | U99 |")),
        ("unlisted header esp_https_ota.h", lambda r: edit(r, "main/main.cpp", "\n", "\n#include <esp_https_ota.h>\n")),
        ("unlisted header rom/ets_sys.h", lambda r: edit(r, "main/main.cpp", "\n", "\n#include \"rom/ets_sys.h\"\n")),
        ("unlisted header services/gap/ble_svc_gap.h", lambda r: edit(r, "main/net.cpp", "\n", "\n#include <services/gap/ble_svc_gap.h>\n")),
        ("stale header hal/uart_ll.h", lambda r: strip_includes(r, "hal/uart_ll.h")),
        ("header row maps to missing used row U98", lambda r: edit(r, doc, "| `driver/gpio.h` | U22 |", "| `driver/gpio.h` | U98 |")),
        (None, lambda r: edit(r, "main/main.cpp", "\n", "\n/*\n#include <esp_https_ota.h>\n*/\n#include <cstdint>\n#include <sys/time.h>\n")),
    ]


def self_test(source: Path) -> int:
    canaries = 0
    with tempfile.TemporaryDirectory(prefix="esp-idf-matrix-selftest-") as directory:
        baseline = Path(directory) / "baseline"
        fixture(source, baseline)
        findings, _ = validate(baseline)
        if findings:
            raise MatrixError("self-test baseline is not clean:\n" + "\n".join(findings))
        for index, (expected, mutate) in enumerate(mutations(baseline)):
            tree = Path(directory) / f"mutant-{index}"
            shutil.copytree(baseline, tree)
            mutate(tree)
            try:
                found, _ = validate(tree)
            except MatrixError as exc:
                found = [str(exc)]
            if expected is None:
                if found:
                    raise MatrixError(f"clean control produced findings: {found}")
            elif not any(expected in finding for finding in found):
                raise MatrixError(f"mutation canary not caught: {expected!r}; got {found}")
            canaries += 1
    return canaries


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        findings, counts = validate(root)
        canaries = self_test(root) if args.self_test and not findings else 0
    except MatrixError as exc:
        print(f"esp-idf-matrix: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:  # A checker fault must not read as ordinary drift.
        print(f"esp-idf-matrix: internal error: {exc!r}", file=sys.stderr)
        return 2
    if findings:
        print("esp-idf-matrix: FAIL — update docs/ESP_IDF_MATRIX.md:", file=sys.stderr)
        for finding in findings:
            print(f"  - {finding}", file=sys.stderr)
        return 1
    print(f"esp-idf-matrix: PASS ({counts['used']} used, {counts['evaluated']} evaluated, "
          f"{counts['components']} CMake + {counts['managed']} managed components, "
          f"{counts['config']} sdkconfig assignments, {counts['headers']} headers"
          + (f", {canaries} self-test cases" if args.self_test else "") + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
