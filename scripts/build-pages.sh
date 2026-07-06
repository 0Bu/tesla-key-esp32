#!/usr/bin/env bash
# Assemble the GitHub Pages site for the ESP Web Tools installer.
#
# The installer (index.html) fetches the firmware in the BROWSER. GitHub release
# assets are served without CORS headers, so the manifest cannot point at release
# URLs cross-origin — the parts must be same-origin with the page. This script
# therefore copies the per-target bins (staged in _fw/<target>/ by ci-build-all.sh)
# into the publish dir alongside the page and writes a manifest.json with one build
# per chipFamily and relative paths. esp-web-tools auto-selects the build matching the
# connected chip. The canonical downloads still live on the GitHub release; these are
# an in-build same-origin mirror and are NOT committed to git.
#
# bootloader / partition-table / app are flashed as SEPARATE parts at their own
# offsets, so a USB (re)flash never overwrites the nvs partition (0x9000) → WiFi / VIN /
# key survive a same-layout reflash. The bootloader offset is per-target: 0x1000 on the
# classic esp32, 0x0 on esp32s3 / esp32c3 / esp32c6.
#
# ONE manifest + per-target image serves every supported chip — a single installer page
# and a single OTA channel (the device pulls tesla-key-esp32[-<s3|c3|c6>].bin for its chip).
#
# Usage:  ./scripts/build-pages.sh <out_dir> <version>
set -euo pipefail

out="${1:?usage: build-pages.sh <out_dir> <version>}"
version="${2:?usage: build-pages.sh <out_dir> <version>}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fw="$repo_root/_fw"
docs="$repo_root/docs"

# target -> esp-web-tools chipFamily string.
chip_family() {
  case "$1" in
    esp32)   echo "ESP32" ;;
    esp32s3) echo "ESP32-S3" ;;
    esp32c3) echo "ESP32-C3" ;;
    esp32c6) echo "ESP32-C6" ;;
    *)       echo "" ;;
  esac
}
# target -> 2nd-stage bootloader flash offset (bytes). Classic esp32 = 0x1000, else 0x0.
boot_offset() { case "$1" in esp32) echo 4096 ;; *) echo 0 ;; esac; }
# target -> short app-image suffix so "esp32" appears once: esp32 -> "" (tesla-key-esp32.bin),
# esp32s3 -> "-s3", esp32c3 -> "-c3", esp32c6 -> "-c6". This names the OTA-served Pages copy,
# so it MUST match TESLA_OTA_IMG_SUFFIX in main/ota_update.cpp and image_suffix() in
# ci-build-all.sh — the device builds the same filename to pull its image.
image_suffix() {
  case "$1" in
    esp32)   echo "" ;;
    esp32s3) echo "-s3" ;;
    esp32c3) echo "-c3" ;;
    esp32c6) echo "-c6" ;;
    # Fail hard like the firmware's #error (ota_update.cpp) — a silently invented
    # suffix would publish an image no device ever builds a filename for.
    *)       echo "image_suffix: unknown target '$1'" >&2; exit 1 ;;
  esac
}

TARGETS="esp32 esp32s3 esp32c3 esp32c6"

rm -rf "$out"
mkdir -p "$out"

# The page itself + any served docs (README / SECURITY), but never a stale manifest.
cp "$docs/index.html" "$out/"
for md in "$docs"/*.md; do
  [[ -e "$md" ]] && cp "$md" "$out/"
done

builds=""
for t in $TARGETS; do
  d="$fw/$t"
  if [[ ! -d "$d" ]]; then
    echo "warn: no staged bins for $t in $d — skipping" >&2
    continue
  fi
  cf="$(chip_family "$t")"
  bo="$(boot_offset "$t")"
  sfx="$(image_suffix "$t")"
  cp "$d/bootloader.bin"      "$out/bootloader-$t.bin"
  cp "$d/partition-table.bin" "$out/partition-table-$t.bin"
  cp "$d/tesla-key-esp32.bin" "$out/tesla-key-esp32$sfx.bin"
  entry=$(cat <<JSON
    {
      "chipFamily": "$cf",
      "parts": [
        { "path": "bootloader-$t.bin", "offset": $bo },
        { "path": "partition-table-$t.bin", "offset": 32768 },
        { "path": "tesla-key-esp32$sfx.bin", "offset": 131072 }
      ]
    }
JSON
)
  builds="${builds:+$builds,
}$entry"
done

if [[ -z "$builds" ]]; then
  echo "error: no per-target bins staged in $fw — run ci-build-all.sh first" >&2
  exit 1
fi

cat > "$out/manifest.json" <<JSON
{
  "name": "tesla-key-esp32",
  "version": "${version}",
  "new_install_prompt_erase": true,
  "builds": [
$builds
  ]
}
JSON

echo "Built Pages site in '$out' for version ${version}:"
ls -1 "$out"
