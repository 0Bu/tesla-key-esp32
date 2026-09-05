#!/usr/bin/env bash
# Build every supported target without access to the OTA private key. The output is deliberately
# data-only: a later trusted job may sign these exact app bytes, but this job can safely execute PR
# code and third-party build tooling without exposing a publishing token or signing secret.
#
# Usage: ./scripts/ci-build-all.sh <display-version> [source-sha]
set -euo pipefail

# Compiler include/dependency environment variables are invisible in compile_commands.json and can
# therefore inject code around the reviewed source and Ninja dependency inventories.  Presence is
# forbidden even when the value is empty so every build starts from the same explicit toolchain.
for variable in \
  CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH OBJC_INCLUDE_PATH \
  DEPENDENCIES_OUTPUT SUNPRO_DEPENDENCIES GCC_EXEC_PREFIX COMPILER_PATH LIBRARY_PATH; do
  if declare -p "$variable" &>/dev/null; then
    echo "ERROR: compiler-injection environment variable is set: $variable" >&2
    exit 2
  fi
done

# ESP-IDF can launch compilation through ccache even though compile_commands.json records the
# target compiler directly. Any caller-provided ccache option could thus wrap or rewrite the real
# compile outside the semantic gate. Reject every present CCACHE_* shell variable, including empty
# values and future options, and disable the invisible launcher for authoritative gate builds.
caller_ccache_variables=()
while IFS= read -r variable; do
  [[ -n "$variable" ]] && caller_ccache_variables+=("$variable")
done < <(compgen -A variable CCACHE_)
if (( ${#caller_ccache_variables[@]} != 0 )); then
  echo "ERROR: caller-provided ccache variables are forbidden:" >&2
  printf '  %s\n' "${caller_ccache_variables[@]}" >&2
  exit 2
fi
unset caller_ccache_variables
# The pinned official IDF image enables ccache by default. Override that image-owned value before
# even the script self-test; unlike caller-controlled CCACHE_* options, its presence is expected.
export IDF_CCACHE_ENABLE=0

VERSION_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$'

validate_inputs() {
  local candidate_version="$1"
  local candidate_sha="$2"

  # `local/local` is the one explicit non-release development stamp.  Every artifact that could
  # flow into Pages uses exactly the browser manifest's version and provenance grammar.
  if [[ "$candidate_version" == local && "$candidate_sha" == local ]]; then
    return 0
  fi
  [[ "$candidate_version" =~ $VERSION_RE ]] || {
    echo "invalid display version (expected X.Y.Z or X.Y.Z-prerelease): $candidate_version" >&2
    return 2
  }
  (( ${#candidate_version} <= 31 )) || {
    echo "invalid display version (must fit the 31-byte ESP app descriptor): $candidate_version" >&2
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
  if validate_inputs 01.2.3 "$valid_sha" >/dev/null 2>&1; then
    echo "self-test failed: non-canonical leading-zero version accepted" >&2
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
  if validate_inputs 1.2.3-this-version-cannot-fit-in-app-desc "$valid_sha" >/dev/null 2>&1; then
    echo "self-test failed: overlong ESP app descriptor version accepted" >&2
    exit 1
  fi
  echo "ci-build-all input self-test passed"
  exit 0
fi

target_override=""
if [[ "${1:-}" == --target ]]; then
  shift
  target_override="${1:?usage: ci-build-all.sh [--target <target>] <display-version> [source-sha]}"
  shift
  case "$target_override" in
    esp32|esp32s3|esp32c3|esp32c6) ;;
    *) echo "unsupported target: $target_override" >&2; exit 2 ;;
  esac
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
# GCC's -fstack-usage sidecars are diagnostic-only and do not alter firmware bytes. ESP-IDF 5.5
# deliberately exposes EXTRA_CFLAGS/EXTRA_CXXFLAGS for high-priority additions; ordinary CFLAGS
# are replaced by its response-file toolchain and would silently produce no sidecars.  Overwrite
# rather than append any caller value so the only effective addition is the reviewed diagnostic.
export EXTRA_CFLAGS="-fstack-usage"
export EXTRA_CXXFLAGS="-fstack-usage"

# Validate the immutable source layout before spending time in the compiler. The generated binary
# is checked against the same exact contract for every target below.
python3 scripts/check-dependency-contract.py --root .
python3 scripts/check-partition-contract.py --csv partitions.csv

echo "ccache disabled for effective-compiler/dependency gate visibility"

TARGETS="esp32 esp32s3 esp32c3 esp32c6"
if [[ -n "$target_override" ]]; then
  TARGETS="$target_override"
fi
APP_POLICY_LIMIT=$((0x1e8000))
SIGNATURE_ALIGNMENT=$((0x10000))
SIGNATURE_SECTOR=$((0x1000))

if [[ -z "$target_override" ]]; then
  rm -rf _unsigned dist
  mkdir -p _unsigned dist
else
  rm -rf "_unsigned/$target_override" "dist/$target_override"
  mkdir -p "_unsigned/$target_override" "dist/$target_override"
fi

# EXACT_FOUR_TARGETS_BEGIN build
for target in $TARGETS; do
  echo "::group::build $target"
  target_started_seconds=$SECONDS
  lock="dependencies.lock.$target"
  if [[ ! -f "$lock" ]]; then
    echo "ERROR: $lock is missing. Resolve intentionally with:" >&2
    echo "  scripts/idf-docker.sh idf.py set-target $target update-dependencies" >&2
    exit 1
  fi
  lock_before="$(sha256sum "$lock" | cut -d' ' -f1)"

  rm -f sdkconfig sdkconfig.old
  # The CLI display version is the authoritative app-descriptor input. GitHub happens to write
  # the same value to version.txt before entering the container, but this script is also the
  # documented local/agent entry point and must not silently fall back to the repository floor.
  # Pass it on every configuring invocation so a stale CMake cache cannot retain another stamp.
  idf.py -D "PROJECT_VER=$version" set-target "$target"
  python3 scripts/check-sdkconfig-defaults.py --target "$target"
  idf.py -D "PROJECT_VER=$version" build

  python3 scripts/check-otadata-contract.py build/ota_data_initial.bin
  python3 scripts/check-build-semantics.py \
    --target "$target" --sdkconfig sdkconfig \
    --compile-commands build/compile_commands.json --source-root "$repo_root"
  python3 scripts/check-partition-contract.py \
    --csv partitions.csv --binary build/partition_table/partition-table.bin
  python3 scripts/check-firmware-artifacts.py \
    --target "$target" --version "$version" \
    --bootloader build/bootloader/bootloader.bin --app build/tesla-key-esp32.bin

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
  python3 scripts/check-stack-usage.py \
    --target "$target" --stack-root build/esp-idf/main \
    --write-observed "$diagnostic/stack-usage-$target.json"
  cp build/bootloader/bootloader.bin "$input/bootloader/bootloader.bin"
  cp build/partition_table/partition-table.bin "$input/partition_table/partition-table.bin"
  cp build/tesla-key-esp32.bin "$input/tesla-key-esp32.bin"
  cp build/ota_data_initial.bin "$input/ota_data_initial.bin"

  cp build/tesla-key-esp32.elf "$diagnostic/tesla-key-esp32-$target.elf"
  sha256sum "$diagnostic/tesla-key-esp32-$target.elf" > "$diagnostic/tesla-key-esp32-$target.elf.sha256"
  cp build/tesla-key-esp32.map "$diagnostic/tesla-key-esp32-$target.map"
  cp sdkconfig "$diagnostic/sdkconfig.$target"
  cp "$lock" "$diagnostic/$lock"
  python -m esp_idf_size --format json build/tesla-key-esp32.map \
    > "$diagnostic/size-$target.json"
  printf '%s\n' "$projected_signed_size" > "$diagnostic/projected-signed-size.txt"
  python3 scripts/report-firmware-size.py \
    --idf-size "$diagnostic/size-$target.json" \
    --unsigned-app "$input/tesla-key-esp32.bin" \
    --projected-signed-size "$projected_signed_size" \
    --policy-limit "$APP_POLICY_LIMIT" \
    --target "$target" > "$diagnostic/size-$target.md"
  target_elapsed_seconds=$((SECONDS - target_started_seconds))
  # Elapsed wall time is useful log evidence but is intentionally not a distributable artifact:
  # embedding it in dist would make otherwise byte-identical builds produce different inventories.
  echo "build-time $target: ${target_elapsed_seconds}s (compile + target gates + reports)"
  echo "::endgroup::"
done
# EXACT_FOUR_TARGETS_END build

# Build and retain diagnostics for every supported target before enforcing reviewed resource
# maxima. This keeps a failed baseline actionable without weakening the gate or silently skipping
# later targets. Baseline updates remain explicit repository changes followed by a clean rerun.
# EXACT_FOUR_TARGETS_BEGIN budget
for target in $TARGETS; do
  input="_unsigned/$target"
  diagnostic="dist/$target"
  projected_signed_size="$(tr -d '[:space:]' < "$diagnostic/projected-signed-size.txt")"
  python3 scripts/report-firmware-size.py \
    --idf-size "$diagnostic/size-$target.json" \
    --unsigned-app "$input/tesla-key-esp32.bin" \
    --projected-signed-size "$projected_signed_size" \
    --policy-limit "$APP_POLICY_LIMIT" \
    --target "$target" \
    --budget-baseline scripts/firmware-size-baseline.json \
    --enforce-budget > "$diagnostic/size-$target.md"
  python3 scripts/check-stack-usage.py \
    --target "$target" \
    --observed-json "$diagnostic/stack-usage-$target.json" \
    --baseline scripts/firmware-stack-baseline.json
done
# EXACT_FOUR_TARGETS_END budget

if [[ -z "$target_override" ]]; then
{
  printf 'head_sha=%s\n' "$source_sha"
  printf 'display_version=%s\n' "$version"
} > dist/build-metadata.txt

# This is a builder claim until the protected signer independently inspects the expected checkout,
# verifies the exact path/size/digest inventory and copies it into signer-owned private staging.
inventory_source_sha="$source_sha"
if [[ "$inventory_source_sha" == local ]]; then
  inventory_source_sha="$(git -c core.fsmonitor=false rev-parse HEAD)"
fi
python3 scripts/check-build-artifact-inventory.py \
  --write --artifact-root . --source-root . \
  --expected-source-sha "$inventory_source_sha" --version "$version"
fi

echo "Built unsigned targets: $TARGETS"
find _unsigned dist -maxdepth 3 -type f -print | sort
command -v ccache >/dev/null 2>&1 && { echo "::group::ccache stats"; ccache -s; echo "::endgroup::"; } || true
