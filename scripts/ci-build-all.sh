#!/usr/bin/env bash
# Build every supported target without access to the OTA private key. The output is deliberately
# data-only: a later trusted job may sign these exact app bytes, but this job can safely execute PR
# code and third-party build tooling without exposing a publishing token or signing secret.
#
# Usage: ./scripts/ci-build-all.sh <display-version> [source-sha]
set -euo pipefail

validate_inputs() {
  local candidate_version="$1"
  local candidate_sha="$2"

  # `local/local` is the one explicit non-release development stamp.  Every artifact that could
  # flow into Pages uses exactly the browser manifest's version and provenance grammar.
  if [[ "$candidate_version" == local && "$candidate_sha" == local ]]; then
    return 0
  fi
  [[ "$candidate_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || {
    echo "invalid display version (expected X.Y.Z or X.Y.Z-prerelease): $candidate_version" >&2
    return 2
  }
  [[ "$candidate_sha" =~ ^[0-9a-f]{40}$ ]] || {
    echo "invalid source SHA (release builds require 40 lowercase hex characters): $candidate_sha" >&2
    return 2
  }
}

if [[ "${1:-}" == --self-test ]]; then
  valid_sha=0123456789abcdef0123456789abcdef01234567
  validate_inputs 1.4.73-PR-228 "$valid_sha"
  validate_inputs local local
  if validate_inputs 1.4.73 not-a-sha >/dev/null 2>&1; then
    echo "self-test failed: invalid SHA accepted" >&2
    exit 1
  fi
  if validate_inputs 'bad version' "$valid_sha" >/dev/null 2>&1; then
    echo "self-test failed: invalid version accepted" >&2
    exit 1
  fi
  if validate_inputs release-1 "$valid_sha" >/dev/null 2>&1; then
    echo "self-test failed: browser-incompatible version accepted" >&2
    exit 1
  fi
  if validate_inputs 1.4.73 local >/dev/null 2>&1; then
    echo "self-test failed: release version accepted without source provenance" >&2
    exit 1
  fi
  if validate_inputs local "$valid_sha" >/dev/null 2>&1; then
    echo "self-test failed: local version accepted with release provenance" >&2
    exit 1
  fi
  echo "ci-build-all input self-test passed"
  exit 0
fi

version="${1:?usage: ci-build-all.sh <display-version> [source-sha]}"
# GitHub Actions does not reliably forward step-level environment variables through container
# actions. CI therefore passes the producing commit explicitly; local builds retain a clear,
# non-provenance marker when the optional argument is omitted.
source_sha="${2:-local}"
validate_inputs "$version" "$source_sha"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
unset IDF_TARGET

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

{
  printf 'head_sha=%s\n' "$source_sha"
  printf 'display_version=%s\n' "$version"
} > dist/build-metadata.txt

echo "Built unsigned targets: $TARGETS"
find _unsigned dist -maxdepth 3 -type f -print | sort
command -v ccache >/dev/null 2>&1 && { echo "::group::ccache stats"; ccache -s; echo "::endgroup::"; } || true
