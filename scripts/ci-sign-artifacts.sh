#!/usr/bin/env bash
# Sign data-only CI build outputs and assemble release/Pages artifacts. This script must run only
# from trusted default-branch code; it never compiles or executes anything from the input artifact.
#
# Usage: ./scripts/ci-sign-artifacts.sh <display-version> [unsigned-dir]
set -euo pipefail

version="${1:?usage: ci-sign-artifacts.sh <display-version> [unsigned-dir]}"
unsigned_dir="${2:-_unsigned}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

case "$version" in
  *[!0-9A-Za-z.+-]*|'') echo "invalid display version: $version" >&2; exit 2 ;;
esac
[[ -d "$unsigned_dir" ]] || { echo "unsigned input directory not found: $unsigned_dir" >&2; exit 1; }

signing_key="${OTA_SIGNING_KEY_FILE:-$repo_root/ota_signing_key.pem}"
[[ -f "$signing_key" && ! -L "$signing_key" ]] || {
  echo "ERROR: signing key missing or not a regular file: $signing_key" >&2
  exit 1
}

TARGETS="esp32 esp32s3 esp32c3 esp32c6"
APP_POLICY_LIMIT=$((0x1e8000))

image_suffix() {
  case "$1" in
    esp32) echo "" ;; esp32s3) echo "-s3" ;; esp32c3) echo "-c3" ;; esp32c6) echo "-c6" ;;
    *) echo "unknown target: $1" >&2; exit 2 ;;
  esac
}

boot_offset() { [[ "$1" == esp32 ]] && echo 0x1000 || echo 0x0; }
flash_frequency() { [[ "$1" == esp32 ]] && echo 40m || echo 80m; }

rm -rf _fw _signed
mkdir -p _fw _signed
# Exact release output names only; never broaden this cleanup to arbitrary user files.
rm -f tesla-key-esp32.bin tesla-key-esp32-s3.bin tesla-key-esp32-c3.bin tesla-key-esp32-c6.bin
rm -f tesla-key-esp32-*.bin

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
  signed_size="$(wc -c < "$work/tesla-key-esp32.bin" | tr -d ' ')"
  (( signed_size <= APP_POLICY_LIMIT )) || {
    echo "ERROR: $target signed app is $signed_size B, over policy $APP_POLICY_LIMIT B" >&2
    exit 1
  }

  cp "$work/bootloader/bootloader.bin" "_fw/$target/bootloader.bin"
  cp "$work/partition_table/partition-table.bin" "_fw/$target/partition-table.bin"
  cp "$work/tesla-key-esp32.bin" "_fw/$target/tesla-key-esp32.bin"

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

echo "Signed release artifacts:"
ls -1 tesla-key-esp32*.bin
