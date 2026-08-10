#!/usr/bin/env bash
# Build one target twice from clean state and compare the unsigned app + unstripped ELF bytes.
# Run inside the pinned ESP-IDF container; signing is intentionally outside this contract.
set -euo pipefail

target="${1:?usage: check-reproducible-build.sh <esp32|esp32s3|esp32c3|esp32c6>}"
case "$target" in esp32|esp32s3|esp32c3|esp32c6) ;; *) echo "unsupported target: $target" >&2; exit 2 ;; esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
unset IDF_TARGET
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

build_once() {
  local pass="$1"
  rm -f sdkconfig sdkconfig.old
  idf.py fullclean >/dev/null 2>&1 || true
  idf.py set-target "$target"
  python3 scripts/check-sdkconfig-defaults.py --target "$target"
  idf.py build
  cp build/tesla-key-esp32.bin "$tmp/app-$pass.bin"
  cp build/tesla-key-esp32.elf "$tmp/app-$pass.elf"
}

build_once a
build_once b
cmp "$tmp/app-a.bin" "$tmp/app-b.bin"
cmp "$tmp/app-a.elf" "$tmp/app-b.elf"
sha256sum "$tmp/app-b.bin" "$tmp/app-b.elf"
echo "reproducible-build $target: PASS (unsigned app + ELF are byte-identical)"
