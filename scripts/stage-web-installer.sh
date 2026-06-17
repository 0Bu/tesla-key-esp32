#!/usr/bin/env bash
# Copy the freshly built firmware parts into docs/ so the ESP Web Tools installer
# (docs/index.html + docs/manifest.json) can serve them.
#
# The installer flashes bootloader / partition-table / app as SEPARATE parts at
# their own offsets, so an UPDATE never overwrites the nvs partition (0x9000) and
# WiFi / VIN / key are preserved. Run this after `idf.py build`.
#
# Usage:  ./scripts/stage-web-installer.sh
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$repo_root/build"
docs="$repo_root/docs"
version="$(tr -d '[:space:]' < "$repo_root/version.txt")"

for f in bootloader/bootloader.bin partition_table/partition-table.bin tesla-key-esp32.bin; do
  if [[ ! -f "$build/$f" ]]; then
    echo "error: $build/$f not found — run 'idf.py build' first" >&2
    exit 1
  fi
done

cp "$build/bootloader/bootloader.bin"             "$docs/bootloader.bin"
cp "$build/partition_table/partition-table.bin"   "$docs/partition-table.bin"
cp "$build/tesla-key-esp32.bin"                   "$docs/tesla-key-esp32-${version}.bin"

echo "Staged into docs/ for version ${version}:"
echo "  bootloader.bin"
echo "  partition-table.bin"
echo "  tesla-key-esp32-${version}.bin"

echo "Make sure docs/manifest.json references tesla-key-esp32-${version}.bin as the app part."
