#!/usr/bin/env bash
# Compile the ownership/OOM gate against the exact cJSON source the firmware links.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ESP-IDF 6 no longer bundles cJSON: the firmware links the espressif/cjson registry release that
# every dependencies.lock.<target> pins by exact version and component hash. Read that single
# resolution, fetch the same release archive and require its Component Manager directory hash to
# equal the locked one, so these bytes are bound to the firmware's (no second pin to drift).
cjson_registry="https://components.espressif.com/"
cjson_lock="$(python3 - "$repo_root" "$cjson_registry" <<'PY'
import pathlib
import re
import sys

root, registry = pathlib.Path(sys.argv[1]), sys.argv[2]
resolutions = set()
for target in ("esp32", "esp32s3", "esp32c3", "esp32c6"):
    text = (root / f"dependencies.lock.{target}").read_text(encoding="utf-8")
    block = re.search(r"^  espressif/cjson:\n((?:    .*\n)+)", text, re.MULTILINE)
    if block is None:
        sys.exit(f"dependencies.lock.{target} has no espressif/cjson resolution")
    fields = dict(re.findall(r"^    (component_hash|version): (\S+)$", block.group(1), re.MULTILINE))
    source = dict(re.findall(r"^      (registry_url|type): (\S+)$", block.group(1), re.MULTILINE))
    if source.get("registry_url") != registry or source.get("type") != "service":
        sys.exit(f"dependencies.lock.{target} espressif/cjson is not the reviewed registry source")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(~[0-9]+)?", fields.get("version", "")):
        sys.exit(f"dependencies.lock.{target} espressif/cjson is not pinned to an exact release")
    if not re.fullmatch(r"[0-9a-f]{64}", fields.get("component_hash", "")):
        sys.exit(f"dependencies.lock.{target} espressif/cjson has no component hash")
    resolutions.add((fields["version"], fields["component_hash"]))
if len(resolutions) != 1:
    sys.exit(f"target lockfiles disagree on espressif/cjson: {sorted(resolutions)}")
print(*resolutions.pop())
PY
)"
cjson_version="${cjson_lock% *}"
cjson_hash="${cjson_lock#* }"
cjson_cache="${TMPDIR:-/tmp}/tesla-cjson-$cjson_hash"
# The cache is keyed by the locked hash but still re-hashed on every run, so a stale or edited
# checkout fails closed instead of compiling other bytes.
python3 - "$cjson_registry" "$cjson_version" "$cjson_hash" "$cjson_cache" "cJSON OOM gate" <<'PY'
import hashlib
import json
import pathlib
import shutil
import sys
import urllib.request
import zipfile

registry, version, expected, cache, gate = sys.argv[1:6]
cache = pathlib.Path(cache)
component = cache / "component"


def component_hash(root: pathlib.Path) -> str:
    # The Component Manager's directory hash: each relative path and its SHA-256, in path order.
    sha = hashlib.sha256()
    files = sorted((path for path in root.rglob("*")
                    if path.is_file() and path.name not in (".component_hash", "CHECKSUMS.json")),
                   key=lambda path: path.relative_to(root).as_posix())
    for path in files:
        sha.update(path.relative_to(root).as_posix().encode("utf-8"))
        sha.update(hashlib.sha256(path.read_bytes()).hexdigest().encode("utf-8"))
    return sha.hexdigest()


if not (cache / ".complete").is_file():
    shutil.rmtree(cache, ignore_errors=True)
    component.mkdir(parents=True)
    with urllib.request.urlopen(f"{registry}api/components/espressif/cjson", timeout=60) as reply:
        entries = [entry for entry in json.load(reply).get("versions", [])
                   if entry.get("version") == version]
    if (len(entries) != 1 or entries[0].get("yanked_at") or
            entries[0].get("component_hash") != expected):
        sys.exit(f"{gate}: registry espressif/cjson {version} is missing, yanked or not the locked release")
    url = entries[0].get("url", "")
    if not url.startswith("https://components-file.espressif.com/components/espressif/cjson/"):
        sys.exit(f"{gate}: registry espressif/cjson {version} archive is not on the Espressif file host")
    archive = cache / "component.zip"
    with urllib.request.urlopen(url, timeout=120) as reply, archive.open("wb") as out:
        shutil.copyfileobj(reply, out)
    with zipfile.ZipFile(archive) as bundle:
        for member in bundle.infolist():
            name = pathlib.PurePosixPath(member.filename)
            if name.is_absolute() or ".." in name.parts:
                sys.exit(f"{gate}: registry espressif/cjson archive has an unsafe member {member.filename}")
        bundle.extractall(component)
    archive.unlink()
    (cache / ".complete").touch()
if component_hash(component) != expected:
    sys.exit(f"{gate}: cJSON checkout does not match locked component hash {expected}")
PY
cjson_dir="$cjson_cache/component/cJSON"
cjson_source="$cjson_dir/cJSON.c"
cjson_header="$cjson_dir/cJSON.h"

if [ ! -f "$cjson_source" ] || [ ! -f "$cjson_header" ]; then
    echo "cJSON OOM gate: locked cJSON source/header not found under $cjson_dir" >&2
    exit 1
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/tesla-cjson-oom.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc="${CC:-gcc}"
cxx="${CXX:-g++}"
sanitize_flags=()
if [ "${CJSON_OOM_SANITIZE:-0}" = 1 ]; then
    sanitize_flags=(-fsanitize=address,undefined,leak -fno-sanitize-recover=all)
fi

"$cc" -std=c11 -Wall -Wextra -Werror "${sanitize_flags[@]}" -I"$cjson_dir" \
    -c "$cjson_source" -o "$work/cJSON.o"
"$cxx" -std=c++17 -Wall -Wextra -Werror "${sanitize_flags[@]}" \
    -I"$repo_root/main" -I"$cjson_dir" \
    "$repo_root/test/test_cjson_oom.cpp" "$work/cJSON.o" -lm \
    -o "$work/test_cjson_oom"
"$work/test_cjson_oom"
