#!/usr/bin/env bash
# Sign data-only CI build outputs and assemble release/Pages artifacts. This script must run only
# from trusted default-branch code; it never compiles or executes anything from the input artifact.
#
# Usage: ./scripts/ci-sign-artifacts.sh <version> <unsigned-dir> <source-sha> <source-root> <independent-root>
set -euo pipefail

usage="ci-sign-artifacts.sh <version> <unsigned-dir> <source-sha> <source-root> <independent-root>"
version="${1:?usage: $usage}"
unsigned_input="${2:?usage: $usage}"
expected_source_sha="${3:?usage: $usage}"
source_root="${4:?usage: $usage}"
independent_root="${5:?usage: $usage}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

VERSION_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$'
[[ "$version" =~ $VERSION_RE ]] || {
  echo "invalid display version (expected X.Y.Z or X.Y.Z-prerelease): $version" >&2
  exit 2
}
(( ${#version} <= 31 )) || {
  echo "invalid display version (must fit the 31-byte ESP app descriptor): $version" >&2
  exit 2
}
[[ "$expected_source_sha" =~ ^[0-9a-f]{40}$ ]] || {
  echo "invalid expected source SHA: $expected_source_sha" >&2
  exit 2
}
[[ "$(basename -- "$unsigned_input")" == _unsigned ]] || {
  echo "unsigned input must be the exact _unsigned subtree of an artifact root: $unsigned_input" >&2
  exit 2
}
artifact_root="$(dirname -- "$unsigned_input")"
[[ -d "$artifact_root" && ! -L "$artifact_root" ]] || {
  echo "artifact root is missing, non-directory or symlinked: $artifact_root" >&2
  exit 1
}
[[ -d "$independent_root" && ! -L "$independent_root" ]] || {
  echo "independent artifact root is missing, non-directory or symlinked: $independent_root" >&2
  exit 1
}

temp_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
private_stage="$(mktemp -d "$temp_root/tesla-key-signer-input.XXXXXX")"
chmod 700 "$private_stage"
cleanup_private_stage() {
  local status=$?
  trap - EXIT
  if [[ -n "${private_stage:-}" && "$private_stage" != / && -d "$private_stage" ]]; then
    if ! rm -rf -- "$private_stage"; then
      echo "ERROR: failed to remove signer-owned private input stage: $private_stage" >&2
      exit 1
    fi
  fi
  exit "$status"
}
trap cleanup_private_stage EXIT

TARGETS="esp32 esp32s3 esp32c3 esp32c6"
APP_POLICY_LIMIT=$((0x1e8000))
SIGNATURE_ALIGNMENT=$((0x10000))
SIGNATURE_SECTOR=$((0x1000))

image_suffix() {
  case "$1" in
    esp32) echo "" ;; esp32s3) echo "-s3" ;; esp32c3) echo "-c3" ;; esp32c6) echo "-c6" ;;
    *) echo "unknown target: $1" >&2; exit 2 ;;
  esac
}

boot_offset() { [[ "$1" == esp32 ]] && echo 0x1000 || echo 0x0; }
flash_frequency() { [[ "$1" == esp32 ]] && echo 40m || echo 80m; }

# Complete every key-free check before the first invocation that can read the private key. This
# prevents stale, cross-target or structurally corrupt PR-produced bytes from being validly signed.
# The producer's inventory is not a trusted source attestation. Recompute it from the separately
# selected checkout, reject every extra/missing/symlinked path, then copy through no-follow file
# descriptors into a private stage and rehash that copy before the key boundary.
python3 scripts/check-dependency-contract.py --root "$source_root"
python3 scripts/check-build-artifact-inventory.py \
  --copy-to "$private_stage" --artifact-root "$artifact_root" \
  --compare-to "$independent_root" --source-root "$source_root" \
  --expected-source-sha "$expected_source_sha" --version "$version"
unsigned_dir="$private_stage/_unsigned"
python3 scripts/check-partition-contract.py --csv partitions.csv
# EXACT_FOUR_TARGETS_BEGIN preflight
for target in $TARGETS; do
  source_dir="$unsigned_dir/$target"
  for path in \
    "$source_dir/bootloader/bootloader.bin" \
    "$source_dir/partition_table/partition-table.bin" \
    "$source_dir/tesla-key-esp32.bin" \
    "$source_dir/ota_data_initial.bin"; do
    [[ -f "$path" && ! -L "$path" ]] || { echo "invalid unsigned input: $path" >&2; exit 1; }
  done
  python3 scripts/check-otadata-contract.py "$source_dir/ota_data_initial.bin"
  python3 scripts/check-partition-contract.py \
    --csv partitions.csv --binary "$source_dir/partition_table/partition-table.bin"
  python3 scripts/check-firmware-artifacts.py \
    --target "$target" --version "$version" \
    --bootloader "$source_dir/bootloader/bootloader.bin" \
    --app "$source_dir/tesla-key-esp32.bin"
done
# EXACT_FOUR_TARGETS_END preflight

signing_key="${OTA_SIGNING_KEY_FILE:-$repo_root/ota_signing_key.pem}"
[[ -f "$signing_key" && ! -L "$signing_key" ]] || {
  echo "ERROR: signing key missing or not a regular file: $signing_key" >&2
  exit 1
}

rm -rf _fw _signed
mkdir -p _fw _signed
# Exact release output names only; never broaden this cleanup to arbitrary user files.
rm -f tesla-key-esp32.bin tesla-key-esp32-s3.bin tesla-key-esp32-c3.bin tesla-key-esp32-c6.bin
for suffix in "" -s3 -c3 -c6; do
  rm -f "tesla-key-esp32${suffix}-${version}.bin" \
    "tesla-key-esp32${suffix}-${version}-merged.bin"
done

# EXACT_FOUR_TARGETS_BEGIN sign
for target in $TARGETS; do
  source_dir="$unsigned_dir/$target"
  work="_signed/$target"
  for path in \
    "$source_dir/bootloader/bootloader.bin" \
    "$source_dir/partition_table/partition-table.bin" \
    "$source_dir/tesla-key-esp32.bin" \
    "$source_dir/ota_data_initial.bin"; do
    [[ -f "$path" && ! -L "$path" ]] || { echo "invalid unsigned input: $path" >&2; exit 1; }
  done

  mkdir -p "$work/bootloader" "$work/partition_table" "_fw/$target"
  cp "$source_dir/bootloader/bootloader.bin" "$work/bootloader/bootloader.bin"
  cp "$source_dir/partition_table/partition-table.bin" "$work/partition_table/partition-table.bin"
  cp "$source_dir/ota_data_initial.bin" "$work/ota_data_initial.bin"

  if ! espsecure.py sign_data --version 2 --keyfile "$signing_key" \
      --output "$work/tesla-key-esp32.bin" "$source_dir/tesla-key-esp32.bin"; then
    rm -f "$work/tesla-key-esp32.bin"
    echo "ERROR: signing $target failed; OTA_SIGNING_KEY must be unencrypted RSA-3072." >&2
    exit 1
  fi
  if ! espsecure.py verify_signature --version 2 --keyfile "$signing_key" \
      "$work/tesla-key-esp32.bin"; then
    rm -f "$work/tesla-key-esp32.bin"
    echo "ERROR: cryptographic verification of the freshly signed $target app failed." >&2
    exit 1
  fi
  unsigned_size="$(wc -c < "$source_dir/tesla-key-esp32.bin" | tr -d ' ')"
  expected_signed_size=$((
    (unsigned_size + SIGNATURE_ALIGNMENT - 1) / SIGNATURE_ALIGNMENT * SIGNATURE_ALIGNMENT
    + SIGNATURE_SECTOR
  ))
  signed_size="$(wc -c < "$work/tesla-key-esp32.bin" | tr -d ' ')"
  (( signed_size == expected_signed_size )) || {
    echo "ERROR: $target signed app size is $signed_size B; exact Secure Boot v2 projection is $expected_signed_size B" >&2
    exit 1
  }
  (( signed_size <= APP_POLICY_LIMIT )) || {
    echo "ERROR: $target signed app is $signed_size B, over policy $APP_POLICY_LIMIT B" >&2
    exit 1
  }
  python3 scripts/check-firmware-artifacts.py \
    --target "$target" --version "$version" --signed-app \
    --bootloader "$work/bootloader/bootloader.bin" --app "$work/tesla-key-esp32.bin"

  cp "$work/bootloader/bootloader.bin" "_fw/$target/bootloader.bin"
  cp "$work/partition_table/partition-table.bin" "_fw/$target/partition-table.bin"
  cp "$work/tesla-key-esp32.bin" "_fw/$target/tesla-key-esp32.bin"
  # The browser writes otadata LAST, after every immutable part has downloaded and passed its
  # digest/length check.  Without this file in the trusted staging tree the installer would either
  # boot whatever slot happened to be selected before the flash or have to activate the new slot
  # before all other writes were known-good.
  cp "$work/ota_data_initial.bin" "_fw/$target/ota_data_initial.bin"

  suffix="$(image_suffix "$target")"
  cp "$work/tesla-key-esp32.bin" "tesla-key-esp32$suffix.bin"
  cp "$work/tesla-key-esp32.bin" "tesla-key-esp32$suffix-$version.bin"
  # Do not consume the build artifact's flash_args: it is untrusted PR-controlled data and an
  # argparse response file could inject signer-tool options. The trusted signer owns this layout.
  ( cd "$work" && esptool.py --chip "$target" merge_bin \
      -o "$repo_root/tesla-key-esp32$suffix-$version-merged.bin" \
      --flash_mode dio --flash_freq "$(flash_frequency "$target")" --flash_size keep \
      "$(boot_offset "$target")" bootloader/bootloader.bin \
      0x8000 partition_table/partition-table.bin \
      0xf000 ota_data_initial.bin \
      0x20000 tesla-key-esp32.bin )
  echo "signed + staged $target: $signed_size B"
done
# EXACT_FOUR_TARGETS_END sign

python3 scripts/check-signed-root-inventory.py . --version "$version"
echo "Signed release artifacts:"
ls -1 tesla-key-esp32*.bin
