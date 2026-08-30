#!/usr/bin/env bash
# Exercise the trusted signer and Pages assembler with a disposable RSA-3072 key.  Run inside the
# digest-pinned ESP-IDF container after ci-build-all.sh has produced the four unsigned target trees.
set -euo pipefail

version="${1:?usage: test-release-contract.sh <version> <source-sha> [unsigned-dir]}"
source_sha="${2:?usage: test-release-contract.sh <version> <source-sha> [unsigned-dir]}"
unsigned_dir="${3:-_unsigned}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

VERSION_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$'
[[ "$version" =~ $VERSION_RE ]] || {
  echo "invalid test release version (expected X.Y.Z or X.Y.Z-prerelease): $version" >&2
  exit 2
}
(( ${#version} <= 31 )) || {
  echo "invalid test release version (must fit the 31-byte ESP app descriptor): $version" >&2
  exit 2
}
[[ "$source_sha" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid test release source SHA" >&2; exit 2; }
command -v openssl >/dev/null 2>&1 || { echo "openssl is required for signer contract test" >&2; exit 1; }
command -v espsecure.py >/dev/null 2>&1 || { echo "espsecure.py is required for signer contract test" >&2; exit 1; }

# The key-free preflight must reject both common artifact-confusion classes before a disposable
# key is created or read: a valid image from the wrong target and a stale embedded version.
if python3 scripts/check-firmware-artifacts.py \
    --target esp32c6 --version "$version" \
    --bootloader "$unsigned_dir/esp32c6/bootloader/bootloader.bin" \
    --app "$unsigned_dir/esp32c3/tesla-key-esp32.bin" >/dev/null 2>&1; then
  echo "signer preflight accepted an esp32c3 app in the esp32c6 artifact tree" >&2
  exit 1
fi
if python3 scripts/check-firmware-artifacts.py \
    --target esp32 --version 0.0.0-stale \
    --bootloader "$unsigned_dir/esp32/bootloader/bootloader.bin" \
    --app "$unsigned_dir/esp32/tesla-key-esp32.bin" >/dev/null 2>&1; then
  echo "signer preflight accepted a stale embedded application version" >&2
  exit 1
fi

secret_dir="$(mktemp -d)"
temp_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
site="$(mktemp -d "$temp_root/tesla-key-pages.XXXXXX")"
comparison="$(mktemp -d "$temp_root/tesla-key-disposable-comparison.XXXXXX")"
key="$secret_dir/disposable-test-key.pem"
chmod 700 "$secret_dir"

cleanup() {
  [[ -n "${secret_dir:-}" && "$secret_dir" != / ]] && rm -rf -- "$secret_dir"
  [[ -n "${site:-}" && "$site" != / ]] && rm -rf -- "$site"
  [[ -n "${comparison:-}" && "$comparison" != / ]] && rm -rf -- "$comparison"
  rm -rf -- "$repo_root/_fw" "$repo_root/_signed"
  rm -f -- tesla-key-esp32.bin tesla-key-esp32-s3.bin tesla-key-esp32-c3.bin tesla-key-esp32-c6.bin
  for suffix in "" -s3 -c3 -c6; do
    rm -f -- "tesla-key-esp32${suffix}-${version}.bin" \
      "tesla-key-esp32${suffix}-${version}-merged.bin"
  done
}
trap cleanup EXIT

# This private copy exercises the dual-inventory signer mechanism locally. Production main/PR
# workflows supply a genuinely independent second-runner rebuild; this disposable-key test does
# not claim independent source-to-byte attestation by itself.
[[ "$(basename -- "$unsigned_dir")" == _unsigned ]] || {
  echo "disposable signer test requires an exact _unsigned subtree" >&2; exit 2;
}
artifact_root="$(dirname -- "$unsigned_dir")"
python3 scripts/check-build-artifact-inventory.py \
  --copy-to "$comparison" --artifact-root "$artifact_root" --source-root . \
  --expected-source-sha "$source_sha" --version "$version" >/dev/null

openssl genrsa -out "$key" 3072 >/dev/null 2>&1
chmod 600 "$key"
OTA_SIGNING_KEY_FILE="$key" ./scripts/ci-sign-artifacts.sh \
  "$version" "$unsigned_dir" "$source_sha" . "$comparison"

# EXACT_FOUR_TARGETS_BEGIN verify-signed
for target in esp32 esp32s3 esp32c3 esp32c6; do
  espsecure.py verify_signature --version 2 --keyfile "$key" \
    "_fw/$target/tesla-key-esp32.bin" >/dev/null
  python3 scripts/check-otadata-contract.py "_fw/$target/ota_data_initial.bin" >/dev/null
done
# EXACT_FOUR_TARGETS_END verify-signed

# Prove that the real ESP-IDF verifier rejects a signed image whose authenticated bytes changed.
# The disposable key and mutation stay inside the private temporary directory and are deleted by
# the trap; this is a cryptographic negative canary, not merely a format-parser test.
tampered_signed="$secret_dir/tampered-esp32.bin"
cp "_fw/esp32/tesla-key-esp32.bin" "$tampered_signed"
python3 - "$tampered_signed" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
data[64] ^= 0x01
path.write_bytes(data)
PY
if espsecure.py verify_signature --version 2 --keyfile "$key" \
    "$tampered_signed" >/dev/null 2>&1; then
  echo "espsecure negative canary accepted authenticated-byte tampering" >&2
  exit 1
fi

./scripts/build-pages.sh "$site" "$version" "$source_sha" >/dev/null
python3 scripts/check-pages-manifest.py "$site" --source-sha "$source_sha" --version "$version" >/dev/null
python3 scripts/check-release-pages-bytes.py "$site" "$repo_root" --version "$version" >/dev/null
echo "disposable-key signer + four-target manifest contract: PASS"
