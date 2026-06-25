#!/usr/bin/env bash
# CI: build the firmware for every tesla-ble-supported target and stage the outputs.
#
# The supported set is exactly the targets yoziru/tesla-ble declares in its
# idf_component.yml (esp32 / esp32s3 / esp32c3 / esp32c6) — the ESP-IDF Component
# Manager refuses any other chip, so this list cannot silently drift.
#
# For each target it stages, into _fw/<target>/, the three flashable parts the web
# installer needs (bootloader / partition-table / app), and writes the release
# downloads to the repo root: a stable + a versioned per-target app image, plus a
# single-file merged image. scripts/build-pages.sh then assembles one multi-build
# manifest.json from _fw/, and the device OTA pulls tesla-key-esp32-<target>.bin.
#
# One sequential job (not a matrix) so build-pages.sh sees every target's bins in the
# same workspace without an artifact round-trip. Run INSIDE the espressif/esp-idf
# container (idf.py + esptool.py on PATH).
#
# Usage:  ./scripts/ci-build-all.sh <version>
set -euo pipefail

version="${1:?usage: ci-build-all.sh <version>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# The esp-idf-ci-action exports IDF_TARGET (from its `target:` input) into the environment;
# `idf.py set-target <other>` then aborts with "Target '<x>' specified on command line is not
# consistent with target '<env>' in the environment." Clear it so set-target alone governs the
# target on each loop iteration (harmless when IDF_TARGET is already unset, e.g. local runs).
unset IDF_TARGET

# Keep in sync with tesla-ble's idf_component.yml `targets:` (the Component Manager
# enforces it — an unsupported target fails at dependency resolution, before compile).
TARGETS="esp32 esp32s3 esp32c3 esp32c6"

rm -rf _fw
for t in $TARGETS; do
  echo "::group::build $t"
  # set-target does a fullclean + regenerates sdkconfig from sdkconfig.defaults; drop
  # the previous target's sdkconfig so each build starts from the committed defaults.
  rm -f sdkconfig
  idf.py set-target "$t" build

  # Size gate: fail loudly if the app nears the OTA slot (0x1f0000) so a future growth
  # can't silently break OTA on the binding target (esp32c6, already ~1.83 MB) while the
  # others keep updating. Gate 0x1e0000 = a 64 KB band below the 0x1f0000 (2031616 B) slot.
  app_size=$(wc -c < build/tesla-key-esp32.bin)
  if [ "$app_size" -gt $((0x1e0000)) ]; then
    echo "ERROR: $t app is $app_size B — over the $((0x1e0000)) B gate (64 KB below the" \
         "0x1f0000=2031616 B OTA slot). Shrink the image or enlarge the slots." >&2
    exit 1
  fi
  echo "size-gate $t OK: $app_size B (gate $((0x1e0000)), slot 2031616)"

  mkdir -p "_fw/$t"
  cp build/bootloader/bootloader.bin           "_fw/$t/bootloader.bin"
  cp build/partition_table/partition-table.bin "_fw/$t/partition-table.bin"
  cp build/tesla-key-esp32.bin                 "_fw/$t/tesla-key-esp32.bin"

  # Release downloads (repo root): stable name (also the OTA filename), versioned copy,
  # and a single-file merged image for manual full flashing. merge_bin reads @flash_args,
  # so the per-target bootloader offset (0x1000 on esp32, 0x0 elsewhere) is baked in.
  cp build/tesla-key-esp32.bin "tesla-key-esp32-$t.bin"
  cp build/tesla-key-esp32.bin "tesla-key-esp32-$t-$version.bin"
  ( cd build && esptool.py --chip "$t" merge_bin \
      -o "../tesla-key-esp32-$t-$version-merged.bin" "@flash_args" )
  echo "::endgroup::"
done

echo "Built + staged targets: $TARGETS"
ls -1 _fw/*/ tesla-key-esp32-*.bin
