#!/usr/bin/env bash
# Compile the ownership/OOM gate against the exact cJSON source the firmware links.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ESP-IDF 6 no longer bundles cJSON: the firmware links espressif/cjson from the Git commit that
# every dependencies.lock.<target> pins. Read that single resolution and fetch the same commit, so
# Git's content addressing binds these bytes to the firmware's (no second pin to drift).
cjson_repo="https://github.com/espressif/idf-extra-components.git"
cjson_commit="$(python3 - "$repo_root" "$cjson_repo" <<'PY'
import pathlib
import re
import sys

root, repo = pathlib.Path(sys.argv[1]), sys.argv[2]
resolutions = set()
for target in ("esp32", "esp32s3", "esp32c3", "esp32c6"):
    text = (root / f"dependencies.lock.{target}").read_text(encoding="utf-8")
    block = re.search(r"^  espressif/cjson:\n((?:    .*\n)+)", text, re.MULTILINE)
    if block is None:
        sys.exit(f"dependencies.lock.{target} has no espressif/cjson resolution")
    fields = dict(re.findall(r"^ {4,6}(git|path|type|version): (\S+)$", block.group(1), re.MULTILINE))
    if fields.get("git") != repo or fields.get("path") != "cjson" or fields.get("type") != "git":
        sys.exit(f"dependencies.lock.{target} espressif/cjson is not the reviewed Git source")
    if not re.fullmatch(r"[0-9a-f]{40}", fields.get("version", "")):
        sys.exit(f"dependencies.lock.{target} espressif/cjson is not pinned to a commit")
    resolutions.add(fields["version"])
if len(resolutions) != 1:
    sys.exit(f"target lockfiles disagree on espressif/cjson: {sorted(resolutions)}")
print(resolutions.pop())
PY
)"
# The component vendors upstream cJSON as a Git submodule; follow its gitlink from the locked tree.
cjson_upstream="https://github.com/DaveGamble/cJSON.git"
cjson_cache="${TMPDIR:-/tmp}/tesla-cjson-$cjson_commit"
if [ ! -f "$cjson_cache/.complete" ]; then
    rm -rf "$cjson_cache"
    git init -q "$cjson_cache/component"
    git -C "$cjson_cache/component" fetch -q --depth 1 --filter=blob:none "$cjson_repo" "$cjson_commit"
    git -C "$cjson_cache/component" show FETCH_HEAD:.gitmodules > "$cjson_cache/gitmodules"
    if [ "$(git config -f "$cjson_cache/gitmodules" --get submodule.cjson/cJSON.url)" != "$cjson_upstream" ]; then
        echo "cJSON OOM gate: locked cjson component no longer vendors $cjson_upstream" >&2
        exit 1
    fi
    git -C "$cjson_cache/component" ls-tree FETCH_HEAD cjson/cJSON |
        awk '$2 == "commit" { print $3 }' > "$cjson_cache/submodule-commit"
    git init -q "$cjson_cache/cJSON"
    git -C "$cjson_cache/cJSON" fetch -q --depth 1 "$cjson_upstream" "$(cat "$cjson_cache/submodule-commit")"
    git -C "$cjson_cache/cJSON" checkout -q FETCH_HEAD
    touch "$cjson_cache/.complete"
fi
cjson_submodule_commit="$(cat "$cjson_cache/submodule-commit")"
if ! [[ "$cjson_submodule_commit" =~ ^[0-9a-f]{40}$ ]] ||
   [ "$(git -C "$cjson_cache/component" rev-parse FETCH_HEAD)" != "$cjson_commit" ] ||
   [ "$(git -C "$cjson_cache/cJSON" rev-parse HEAD)" != "$cjson_submodule_commit" ]; then
    echo "cJSON OOM gate: cJSON checkout does not match locked component $cjson_commit" >&2
    exit 1
fi
cjson_dir="$cjson_cache/cJSON"
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
