#!/usr/bin/env bash
# Exercise the trusted signer and Pages assembler with a disposable RSA-3072 key.  Run inside the
# digest-pinned ESP-IDF container after ci-build-all.sh has produced the four unsigned target trees.
set -euo pipefail

version="${1:?usage: test-release-contract.sh <version> <source-sha> [unsigned-dir]}"
source_sha="${2:?usage: test-release-contract.sh <version> <source-sha> [unsigned-dir]}"
unsigned_dir="${3:-_unsigned}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$ ]] || {
  echo "invalid test release version (expected X.Y.Z or X.Y.Z-prerelease): $version" >&2
  exit 2
}
[[ "$source_sha" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid test release source SHA" >&2; exit 2; }
command -v openssl >/dev/null 2>&1 || { echo "openssl is required for signer contract test" >&2; exit 1; }
command -v espsecure.py >/dev/null 2>&1 || { echo "espsecure.py is required for signer contract test" >&2; exit 1; }

secret_dir="$(mktemp -d)"
temp_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
site="$(mktemp -d "$temp_root/tesla-key-pages.XXXXXX")"
key="$secret_dir/disposable-test-key.pem"
chmod 700 "$secret_dir"

cleanup() {
  [[ -n "${secret_dir:-}" && "$secret_dir" != / ]] && rm -rf -- "$secret_dir"
  [[ -n "${site:-}" && "$site" != / ]] && rm -rf -- "$site"
  rm -rf -- "$repo_root/_fw" "$repo_root/_signed"
  rm -f -- tesla-key-esp32.bin tesla-key-esp32-s3.bin tesla-key-esp32-c3.bin tesla-key-esp32-c6.bin
  for suffix in "" -s3 -c3 -c6; do
    rm -f -- "tesla-key-esp32${suffix}-${version}.bin" \
      "tesla-key-esp32${suffix}-${version}-merged.bin"
  done
}
trap cleanup EXIT

openssl genrsa -out "$key" 3072 >/dev/null 2>&1
chmod 600 "$key"
OTA_SIGNING_KEY_FILE="$key" ./scripts/ci-sign-artifacts.sh "$version" "$unsigned_dir"

for target in esp32 esp32s3 esp32c3 esp32c6; do
  espsecure.py verify_signature --version 2 --keyfile "$key" \
    "_fw/$target/tesla-key-esp32.bin" >/dev/null
  [[ "$(wc -c < "_fw/$target/ota_data_initial.bin" | tr -d ' ')" == 8192 ]] || {
    echo "$target signer staging lost/changed ota_data_initial.bin" >&2; exit 1;
  }
done

./scripts/build-pages.sh "$site" "$version" "$source_sha" >/dev/null
python3 scripts/check-pages-manifest.py "$site" --source-sha "$source_sha" --version "$version" >/dev/null
echo "disposable-key signer + four-target manifest contract: PASS"
