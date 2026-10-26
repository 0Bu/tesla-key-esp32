#!/usr/bin/env bash
# Actual maintenance bodies with passive public SDK signatures; no live backend or crypto.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/config-repair-backend.XXXXXX")"
trap 'rm -rf "$work"' EXIT
cxx="${CXX:-c++}"
flags=(-std=c++17 -Wall -Wextra -Werror)
if [ "${CONFIG_REPAIR_SANITIZE:-0}" = 1 ]; then
    flags+=(-fsanitize=address,undefined,leak -fno-omit-frame-pointer -g)
fi
python3 "$repo_root/test/generate_config_repair_backend.py" "$work/config_repair_backend.generated.hpp"
for target in esp32 esp32s3 esp32c3 esp32c6; do
    upper="$(printf '%s' "$target" | tr '[:lower:]' '[:upper:]')"
    "$cxx" "${flags[@]}" "-DCONFIG_IDF_TARGET_${upper}=1" \
        -I"$repo_root/test/config_repair_sdk" -I"$repo_root/main" -I"$work" \
        "$repo_root/test/test_config_repair_backend.cpp" -o "$work/$target"
    for mode in backend-direct run-positive console-positive console-installed console-expired console-zero console-bad-count console-tx-deadline console-entry-installed read-time-progress; do
        "$work/$target" "$mode"
    done
done
for mode in owner-busy owner-late raw-pending raw-late storage-init key-parse key-bits key-refresh wrong-public wrong-challenge journal-present journal-type journal-late cfg-valid cfg-missing length-drift iterator-find iterator-info iterator-next value-overlong unsupported-type hash-setup info-failure read-failure receipt-failure set-visible-failure commit-visible-failure readback-corrupt retention-change readback-substitute retention-length-drift key-journal-late; do
    "$work/esp32s3" "$mode"
done
