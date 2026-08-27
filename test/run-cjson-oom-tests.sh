#!/usr/bin/env bash
# Compile the ownership/OOM gate against the exact cJSON source shipped by the pinned ESP-IDF.
# Run through scripts/idf-docker.sh so IDF_PATH cannot silently resolve to an unpinned host copy.
set -euo pipefail

: "${IDF_PATH:?run inside the pinned ESP-IDF environment}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cjson_dir="$IDF_PATH/components/json/cJSON"
cjson_source="$cjson_dir/cJSON.c"
cjson_header="$cjson_dir/cJSON.h"

if [ ! -f "$cjson_source" ] || [ ! -f "$cjson_header" ]; then
    echo "cJSON OOM gate: pinned ESP-IDF cJSON source/header not found under $cjson_dir" >&2
    exit 1
fi

idf_tag="$(git -C "$IDF_PATH" describe --tags --exact-match HEAD 2>/dev/null || true)"
if [ "$idf_tag" != "v5.5.5" ]; then
    echo "cJSON OOM gate: expected pinned ESP-IDF v5.5.5, got ${idf_tag:-unknown}" >&2
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
