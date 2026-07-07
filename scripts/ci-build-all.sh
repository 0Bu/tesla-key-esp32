#!/usr/bin/env bash
# CI: build the firmware for every supported target and stage the outputs.
#
# esp32 / esp32s3 / esp32c3 / esp32c6 are exactly the targets yoziru/tesla-ble declares in its
# idf_component.yml — the ESP-IDF Component Manager refuses any other chip, so that list cannot
# silently drift. esp32c5 is added on top via a local build-time patch of tesla-ble (its
# upstream manifest omits esp32c5; see prepare-tesla-ble-c5.sh + main/idf_component.yml), so
# the T-Dongle-C5 (dual-band Wi-Fi 6) builds from the same source tree.
#
# For each target it stages, into _fw/<target>/, the three flashable parts the web
# installer needs (bootloader / partition-table / app), and writes the release
# downloads to the repo root: a stable + a versioned per-target app image, plus a
# single-file merged image. scripts/build-pages.sh then assembles one multi-build
# manifest.json from _fw/, and the device OTA pulls its own tesla-key-esp32[-<s3|c3|c6|c5>].bin.
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

# Speed up the four cold per-target compiles with ccache. The espressif/idf image ships
# ccache, and ESP-IDF's build system uses it as the compiler launcher when IDF_CCACHE_ENABLE
# is set. Most of what each target rebuilds is the rarely-changing ESP-IDF framework +
# tesla-ble, identical run-to-run, so a warm cache turns four full rebuilds into mostly
# cache-hit passes. CCACHE_DIR lives under the repo (= the container-mounted GITHUB_WORKSPACE)
# so CI can persist it across runs via actions/cache; absolute paths are stable run-to-run
# (fixed mount point + pinned IDF image), so cache entries stay valid. Guarded on ccache being
# present so a plain local toolchain without it still builds normally.
if command -v ccache >/dev/null 2>&1; then
  export IDF_CCACHE_ENABLE=1
  export CCACHE_DIR="$repo_root/.ccache"
  export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"
  echo "ccache enabled (CCACHE_DIR=$CCACHE_DIR, max $CCACHE_MAXSIZE)"
fi

# esp32/s3/c3/c6 map 1:1 to tesla-ble's idf_component.yml `targets:` (the Component Manager
# enforces it — an unsupported target fails at dependency resolution, before compile). esp32c5
# is NOT in that upstream list; we add it via a local build-time patch (see below +
# scripts/prepare-tesla-ble-c5.sh + main/idf_component.yml), so it builds from ONE source tree
# alongside the other four.
TARGETS="esp32 esp32s3 esp32c3 esp32c6 esp32c5"

# esp32c5 needs the local, patched tesla-ble checkout in place before its dependency resolves
# (the upstream manifest omits esp32c5 → the Component Manager would refuse the chip). Prepare
# it up front; idempotent + a warm checkout is reused, and it is a no-op for the other targets
# (they never reference third_party/tesla-ble).
"$repo_root/scripts/prepare-tesla-ble-c5.sh"

# target -> short image suffix so "esp32" appears once in the release/OTA filename:
# esp32 -> "", esp32s3 -> "-s3", esp32c3 -> "-c3", esp32c6 -> "-c6", esp32c5 -> "-c5".
# Must match TESLA_OTA_IMG_SUFFIX in main/ota_update.cpp (the device builds the same name
# to pull its image) and image_suffix() in build-pages.sh (the OTA-served Pages copy).
image_suffix() {
  case "$1" in
    esp32)   echo "" ;;
    esp32s3) echo "-s3" ;;
    esp32c3) echo "-c3" ;;
    esp32c6) echo "-c6" ;;
    esp32c5) echo "-c5" ;;
    # Fail hard like the firmware's #error (ota_update.cpp) — a silently invented
    # suffix would publish an image no device ever builds a filename for.
    *)       echo "image_suffix: unknown target '$1'" >&2; exit 1 ;;
  esac
}

# OTA image signing (Secure Boot v2 RSA-3072 scheme, no hardware Secure Boot — see
# sdkconfig.defaults + docs/SECURITY.md). The build is configured with
# CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n, so the binary comes out UNSIGNED and we sign it
# here with the OFFLINE key. The private key never lives in the repo: CI writes it from the
# OTA_SIGNING_KEY secret to a transient file (default ota_signing_key.pem in the repo root,
# gitignored) and may point OTA_SIGNING_KEY_FILE at it. Signed in place, BEFORE the size gate
# and copies, so EVERY derived artifact (release .bin, versioned copy, merged image, and the
# _fw/ copy the web installer serves) inherits the signature and a device's
# CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT check passes. With no key we build UNSIGNED and
# warn — fork/PR builds (no secret) still compile; the workflow gates a real release on the key.
signing_key="${OTA_SIGNING_KEY_FILE:-$repo_root/ota_signing_key.pem}"
sign_image() {
  local bin="$1"
  if [ -n "$signing_key" ] && [ -f "$signing_key" ]; then
    # espsecure prints only diagnostics (never key bytes); surface a clear, actionable hint if
    # the key can't be loaded (wrong scheme/EC, encrypted PEM, or mangled-newline secret) so a
    # bad OTA_SIGNING_KEY fails with guidance instead of a raw Python traceback.
    if ! espsecure.py sign_data --version 2 --keyfile "$signing_key" --output "$bin.signed" "$bin"; then
      rm -f "$bin.signed"
      echo "ERROR: signing $bin failed. OTA_SIGNING_KEY must be an UNENCRYPTED RSA-3072" \
           "Secure Boot v2 key. Regenerate with:" \
           "  espsecure.py generate_signing_key --version 2 --scheme rsa3072 ota_signing_key.pem" \
           "and store the FULL PEM (incl. BEGIN/END lines and real newlines) as the secret." >&2
      exit 1
    fi
    mv "$bin.signed" "$bin"
    echo "signed $bin (Secure Boot v2 RSA-3072 app signature)"
  else
    echo "WARNING: no OTA signing key at '$signing_key' — building UNSIGNED ($bin)." \
         "OTA onto a device already on a signed build will be REFUSED." >&2
  fi
}

rm -rf _fw
for t in $TARGETS; do
  echo "::group::build $t"
  # set-target does a fullclean + regenerates sdkconfig from sdkconfig.defaults; drop
  # the previous target's sdkconfig so each build starts from the committed defaults.
  rm -f sdkconfig
  idf.py set-target "$t" build

  # Sign the freshly built app in place so all copies below (and the size gate) see the
  # final, signed image that actually gets flashed/served.
  sign_image build/tesla-key-esp32.bin

  # Size gate: fail loudly if the SIGNED app nears the OTA slot (0x1f0000) so a future growth
  # can't silently break OTA on whichever target grows largest while the others keep updating.
  # The biggest images are esp32c6 and esp32c5 (~0x1d1000 signed). esp32c5 carries the extra
  # on-device display + PSRAM code but still fits at the base -Og — the Package A size levers
  # (#154) freed the ~64 KB it needs. (-Os is deliberately NOT used: whole-build -Os hard-freezes
  # this firmware under evcc+BLE load, rejected Package B — see sdkconfig.defaults.esp32c5.)
  # Every image's code rounds UP to a 64 KB Secure-Boot-v2 boundary and gets a 4 KB signature
  # sector appended. Gate 0x1e8000 = a 32 KB band below the 0x1f0000 (2031616 B) slot: a real
  # early-warning margin, checked here AFTER signing so it sees the exact bytes flashed/served.
  app_size=$(wc -c < build/tesla-key-esp32.bin)
  if [ "$app_size" -gt $((0x1e8000)) ]; then
    echo "ERROR: $t app is $app_size B — over the $((0x1e8000)) B gate (32 KB below the" \
         "0x1f0000=2031616 B OTA slot). Shrink the image or enlarge the slots." >&2
    exit 1
  fi
  echo "size-gate $t OK: $app_size B (gate $((0x1e8000)), slot 2031616)"

  mkdir -p "_fw/$t"
  cp build/bootloader/bootloader.bin           "_fw/$t/bootloader.bin"
  cp build/partition_table/partition-table.bin "_fw/$t/partition-table.bin"
  cp build/tesla-key-esp32.bin                 "_fw/$t/tesla-key-esp32.bin"

  # Release downloads (repo root): stable name (also the OTA filename), versioned copy,
  # and a single-file merged image for manual full flashing. merge_bin reads @flash_args,
  # so the per-target bootloader offset (0x1000 on esp32, 0x2000 on esp32c5, 0x0 on s3/c3/c6)
  # is baked in.
  sfx="$(image_suffix "$t")"
  cp build/tesla-key-esp32.bin "tesla-key-esp32$sfx.bin"
  cp build/tesla-key-esp32.bin "tesla-key-esp32$sfx-$version.bin"
  ( cd build && esptool.py --chip "$t" merge_bin \
      -o "../tesla-key-esp32$sfx-$version-merged.bin" "@flash_args" )
  echo "::endgroup::"
done

echo "Built + staged targets: $TARGETS"
# Glob without the dash so the suffix-less classic-esp32 image (tesla-key-esp32.bin) is listed too.
ls -1 _fw/*/ tesla-key-esp32*.bin

# Surface the ccache hit rate so a cold/poisoned cache is visible in the CI log.
command -v ccache >/dev/null 2>&1 && { echo "::group::ccache stats"; ccache -s; echo "::endgroup::"; } || true
