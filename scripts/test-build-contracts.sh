#!/usr/bin/env bash
# Fast, dependency-free host tests for destructive-path, target and manifest contracts.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
stage="$temp/fw"
mkdir -p "$stage"

make_bytes() {
  local path="$1" count="$2"
  dd if=/dev/zero of="$path" bs=1 count="$count" status=none
}

for target in esp32 esp32s3 esp32c3 esp32c6; do
  mkdir -p "$stage/$target"
  make_bytes "$stage/$target/bootloader.bin" 1024
  make_bytes "$stage/$target/partition-table.bin" 1024
  make_bytes "$stage/$target/tesla-key-esp32.bin" 4096
  make_bytes "$stage/$target/ota_data_initial.bin" 8192
done

sha=0123456789abcdef0123456789abcdef01234567
temp_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}"
site="$(mktemp -d "$temp_root/tesla-key-pages.XXXXXX")"
trap 'rm -rf "$temp" "$site"' EXIT

if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" . 1.2.3 "$sha" >/dev/null 2>&1; then
  echo "build-pages path guard self-test failed: repository root accepted" >&2
  exit 1
fi
if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" release-1 "$sha" >/dev/null 2>&1; then
  echo "build-pages version self-test failed: browser-incompatible version accepted" >&2
  exit 1
fi
if "$repo_root/scripts/test-release-contract.sh" release-1 "$sha" "$stage" >/dev/null 2>&1; then
  echo "release-contract version self-test failed: browser-incompatible version accepted" >&2
  exit 1
fi

FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null
python3 "$repo_root/scripts/check-pages-manifest.py" "$site" --source-sha "$sha" --version 1.2.3 >/dev/null
python3 - "$site/manifest.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
assert d["layoutVersion"] == 2 and len(d["builds"]) == 4
assert all(len(build["parts"]) == 4 and build["parts"][-1]["offset"] == 0xF000 for build in d["builds"])
PY

mv "$stage/esp32c6" "$temp/esp32c6"
if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null 2>&1; then
  echo "build-pages target self-test failed: missing target accepted" >&2
  exit 1
fi
mv "$temp/esp32c6" "$stage/esp32c6"

mkdir "$stage/esp32c5"
if FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null 2>&1; then
  echo "build-pages target self-test failed: unexpected target accepted" >&2
  exit 1
fi
rmdir "$stage/esp32c5"

FIRMWARE_STAGE_DIR="$stage" "$repo_root/scripts/build-pages.sh" "$site" 1.2.3 "$sha" >/dev/null
printf x >> "$site/tesla-key-esp32.bin"
if python3 "$repo_root/scripts/check-pages-manifest.py" "$site" >/dev/null 2>&1; then
  echo "manifest digest/length self-test failed: tampered file accepted" >&2
  exit 1
fi

echo "build/manifest contract self-test: PASS"
