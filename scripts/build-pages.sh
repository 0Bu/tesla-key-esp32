#!/usr/bin/env bash
# Assemble the GitHub Pages site for the ESP Web Tools installer.
#
# The installer (index.html) fetches the firmware in the BROWSER. GitHub release
# assets are served without CORS headers, so the manifest cannot point at release
# URLs cross-origin — the parts must be same-origin with the page. This script
# therefore copies the freshly built bins into the publish dir alongside the page
# and writes a manifest.json with relative paths. The canonical downloads still
# live on the GitHub release; these are an in-build same-origin mirror and are
# NOT committed to git.
#
# bootloader / partition-table / app are flashed as SEPARATE parts at their own
# offsets (0x0 / 0x8000 / 0x20000 = ota_0), so a USB (re)flash never overwrites the
# nvs partition (0x9000) → WiFi / VIN / key survive a same-layout reflash. Note the
# app now lives at 0x20000 (the ota_0 slot) — the dual-OTA partition table moved it
# from the old single 0x10000 factory partition. A full-erase install is required
# once to migrate a device onto this layout (after that, updates happen over OTA).
#
# A board variant whose binary differs from the generic image (display, flash size,
# console — e.g. the LilyGo T-Dongle-S3) needs its OWN OTA channel, or a self-update
# would replace it with the feature-less generic image. Pass its build dir as the 3rd
# arg and a slug as the 4th; this then mirrors that build's bins + a manifest into a
# <slug>/ subdir, which the board firmware's CONFIG_TESLA_OTA_*_URL points at. Same
# <version> as the generic build (both cut from one commit) so one device never sees
# the two channels report different versions.
#
# Usage:  ./scripts/build-pages.sh <out_dir> <version> [board_build_dir] [board_slug]
set -euo pipefail

out="${1:?usage: build-pages.sh <out_dir> <version> [board_build_dir] [board_slug]}"
version="${2:?usage: build-pages.sh <out_dir> <version> [board_build_dir] [board_slug]}"
board_dir="${3:-}"
board_slug="${4:-t-dongle-s3}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$repo_root/build"
docs="$repo_root/docs"

for f in bootloader/bootloader.bin partition_table/partition-table.bin tesla-key-esp32.bin; do
  if [[ ! -f "$build/$f" ]]; then
    echo "error: $build/$f not found — run 'idf.py build' first" >&2
    exit 1
  fi
done

rm -rf "$out"
mkdir -p "$out"

# The page itself + any served docs (README / SECURITY), but never a stale manifest.
cp "$docs/index.html" "$out/"
for md in "$docs"/*.md; do
  [[ -e "$md" ]] && cp "$md" "$out/"
done

# Same-origin firmware mirror for the browser flasher.
cp "$build/bootloader/bootloader.bin"           "$out/bootloader.bin"
cp "$build/partition_table/partition-table.bin" "$out/partition-table.bin"
cp "$build/tesla-key-esp32.bin"                  "$out/tesla-key-esp32.bin"

cat > "$out/manifest.json" <<JSON
{
  "name": "tesla-key-esp32",
  "version": "${version}",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partition-table.bin", "offset": 32768 },
        { "path": "tesla-key-esp32.bin", "offset": 131072 }
      ]
    }
  ]
}
JSON

echo "Built Pages site in '$out' for version ${version}:"
ls -1 "$out"

# ─── Optional per-board OTA channel (e.g. T-Dongle-S3) ─────────────────────────
if [[ -n "$board_dir" ]]; then
  for f in bootloader/bootloader.bin partition_table/partition-table.bin tesla-key-esp32.bin; do
    if [[ ! -f "$board_dir/$f" ]]; then
      echo "error: $board_dir/$f not found — build the '$board_slug' variant first" >&2
      exit 1
    fi
  done
  bout="$out/$board_slug"
  mkdir -p "$bout"
  cp "$board_dir/bootloader/bootloader.bin"           "$bout/bootloader.bin"
  cp "$board_dir/partition_table/partition-table.bin" "$bout/partition-table.bin"
  cp "$board_dir/tesla-key-esp32.bin"                  "$bout/tesla-key-esp32.bin"

  # Offsets match the generic layout (partitions.csv): app at ota_0 (0x20000), so an
  # OTA writes the inactive slot and a USB reflash never touches nvs (0x9000).
  cat > "$bout/manifest.json" <<JSON
{
  "name": "tesla-key-esp32 (${board_slug})",
  "version": "${version}",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partition-table.bin", "offset": 32768 },
        { "path": "tesla-key-esp32.bin", "offset": 131072 }
      ]
    }
  ]
}
JSON

  echo "Built per-board OTA channel in '$bout' for '$board_slug' version ${version}:"
  ls -1 "$bout"
fi
