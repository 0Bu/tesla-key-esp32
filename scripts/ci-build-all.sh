#!/usr/bin/env bash
# Build every supported target without access to the OTA private key. The output is deliberately
# data-only: a later trusted job may sign these exact app bytes, but this job can safely execute PR
# code and third-party build tooling without exposing a publishing token or signing secret.
#
# Usage: ./scripts/ci-build-all.sh <display-version>
set -euo pipefail

version="${1:?usage: ci-build-all.sh <display-version>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
unset IDF_TARGET

case "$version" in
  *[!0-9A-Za-z.+-]*|'') echo "invalid display version: $version" >&2; exit 2 ;;
esac

if command -v ccache >/dev/null 2>&1; then
  export IDF_CCACHE_ENABLE=1
  export CCACHE_DIR="$repo_root/.ccache"
  export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"
  export CCACHE_COMPILERCHECK=content
  echo "ccache enabled (content compiler check, max $CCACHE_MAXSIZE)"
fi

TARGETS="esp32 esp32s3 esp32c3 esp32c6"
APP_POLICY_LIMIT=$((0x1e8000))
SIGNATURE_ALIGNMENT=$((0x10000))
SIGNATURE_SECTOR=$((0x1000))

rm -rf _unsigned dist
mkdir -p _unsigned dist

for target in $TARGETS; do
  echo "::group::build $target"
  lock="dependencies.lock.$target"
  if [[ ! -f "$lock" ]]; then
    echo "ERROR: $lock is missing. Resolve intentionally with:" >&2
    echo "  scripts/idf-docker.sh idf.py set-target $target update-dependencies" >&2
    exit 1
  fi
  lock_before="$(sha256sum "$lock" | cut -d' ' -f1)"

  rm -f sdkconfig sdkconfig.old
  idf.py set-target "$target"
  python3 scripts/check-sdkconfig-defaults.py --target "$target"
  idf.py build

  lock_after="$(sha256sum "$lock" | cut -d' ' -f1)"
  if [[ "$lock_before" != "$lock_after" ]]; then
    echo "ERROR: $lock changed during a normal build. Review and run idf.py" >&2
    echo "update-dependencies for $target deliberately, then commit the resulting lockfile." >&2
    exit 1
  fi

  unsigned_size="$(wc -c < build/tesla-key-esp32.bin | tr -d ' ')"
  projected_signed_size=$(( (unsigned_size + SIGNATURE_ALIGNMENT - 1) / SIGNATURE_ALIGNMENT * SIGNATURE_ALIGNMENT + SIGNATURE_SECTOR ))
  if (( projected_signed_size > APP_POLICY_LIMIT )); then
    echo "ERROR: $target projected signed app is $projected_signed_size B, over policy" >&2
    echo "limit $APP_POLICY_LIMIT B (32 KiB below the 0x1f0000 OTA slot)." >&2
    exit 1
  fi
  echo "size-gate $target OK: unsigned=$unsigned_size B projected-signed=$projected_signed_size B"

  input="_unsigned/$target"
  diagnostic="dist/$target"
  mkdir -p "$input/bootloader" "$input/partition_table" "$diagnostic"
  cp build/bootloader/bootloader.bin "$input/bootloader/bootloader.bin"
  cp build/partition_table/partition-table.bin "$input/partition_table/partition-table.bin"
  cp build/tesla-key-esp32.bin "$input/tesla-key-esp32.bin"
  if [[ -f build/ota_data_initial.bin ]]; then
    cp build/ota_data_initial.bin "$input/ota_data_initial.bin"
  fi

  cp build/tesla-key-esp32.elf "$diagnostic/tesla-key-esp32-$target.elf"
  sha256sum "$diagnostic/tesla-key-esp32-$target.elf" > "$diagnostic/tesla-key-esp32-$target.elf.sha256"
  cp build/tesla-key-esp32.map "$diagnostic/tesla-key-esp32-$target.map"
  cp sdkconfig "$diagnostic/sdkconfig.$target"
  cp "$lock" "$diagnostic/$lock"
  python -m esp_idf_size --format json build/tesla-key-esp32.map \
    > "$diagnostic/size-$target.json"
  python3 scripts/report-firmware-size.py \
    --idf-size "$diagnostic/size-$target.json" \
    --unsigned-app build/tesla-key-esp32.bin \
    --projected-signed-size "$projected_signed_size" \
    --policy-limit "$APP_POLICY_LIMIT" \
    --target "$target" > "$diagnostic/size-$target.md"
  echo "::endgroup::"
done

source_sha="${SOURCE_SHA:-${GITHUB_SHA:-}}"
if [[ -z "$source_sha" ]]; then
  # Docker Desktop cannot follow a host worktree's absolute .git file. CI supplies SOURCE_SHA;
  # local builds use an explicit non-provenance marker without emitting a misleading fatal error.
  source_sha="local"
fi
{
  printf 'head_sha=%s\n' "$source_sha"
  printf 'display_version=%s\n' "$version"
} > dist/build-metadata.txt

echo "Built unsigned targets: $TARGETS"
find _unsigned dist -maxdepth 3 -type f -print | sort
command -v ccache >/dev/null 2>&1 && { echo "::group::ccache stats"; ccache -s; echo "::endgroup::"; } || true
