#!/usr/bin/env bash
# Compile the optional LED shell from the just-verified S3 configuration. Nothing from this
# isolated, unsigned variant enters the fixed publication inventory or changes shipped defaults.
set -euo pipefail

[[ $# -eq 2 ]] || { echo 'usage: ci-build-led-variant.sh <version> <primary-s3-sdkconfig>' >&2; exit 2; }
version="$1"
primary_config="$2"
: "${IDF_PATH:?run through the pinned ESP-IDF container}"
[[ "${OTA_SIGNING_KEY+x}" != x && "${OTA_SIGNING_KEY_FILE+x}" != x ]] || {
  echo 'LED variant must not receive a signing-key variable' >&2; exit 2;
}
[[ -f "$primary_config" && ! -L "$primary_config" ]] || exit 2
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"
mkdir -p .tmp
work="$(mktemp -d "$repo_root/.tmp/led-build.XXXXXX")"
proof="$(mktemp -d "$repo_root/.tmp/led-proof.XXXXXX")"
cleanup() {
  local result=$?
  trap - EXIT
  if [[ "$result" -eq 0 ]]; then
    rm -rf -- "$work"
  else
    echo "LED variant failed; diagnostics retained at $work and $proof" >&2
  fi
  exit "$result"
}
trap cleanup EXIT

python3 - "$primary_config" "$work/sdkconfig" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
assert source.splitlines().count('CONFIG_IDF_TARGET="esp32s3"') == 1
disabled = '# CONFIG_TESLA_LED_ENABLED is not set'
assert source.splitlines().count(disabled) == 1
Path(sys.argv[2]).write_text(source.replace(disabled, 'CONFIG_TESLA_LED_ENABLED=y'))
PY

idf.py -B "$work/build" -D "SDKCONFIG=$work/sdkconfig" -D IDF_TARGET=esp32s3 \
  -D "PROJECT_VER=$version" build
python3 scripts/check-sdkconfig-defaults.py --target esp32s3 --generated "$work/sdkconfig"
python3 - "$work/sdkconfig" <<'PY'
from pathlib import Path
import sys
lines = Path(sys.argv[1]).read_text().splitlines()
for wanted in ('CONFIG_TESLA_LED_ENABLED=y', 'CONFIG_TESLA_LED_DI_GPIO=40',
               'CONFIG_TESLA_LED_CI_GPIO=39', 'CONFIG_TESLA_LED_BRIGHTNESS=8'):
    assert lines.count(wanted) == 1, wanted
PY
python3 scripts/check-build-semantics.py --target esp32s3 --sdkconfig "$work/sdkconfig" \
  --compile-commands "$work/build/compile_commands.json" --source-root "$repo_root"
python3 scripts/check-partition-contract.py --csv partitions.csv \
  --binary "$work/build/partition_table/partition-table.bin"
python3 scripts/check-firmware-artifacts.py --target esp32s3 --version "$version" \
  --bootloader "$work/build/bootloader/bootloader.bin" --app "$work/build/tesla-key-esp32.bin"
python3 scripts/check-stack-usage.py --target esp32s3 --stack-root "$work/build/esp-idf/main" \
  --write-observed "$proof/stack-usage.json"
python3 scripts/check-stack-usage.py --target esp32s3 --observed-json "$proof/stack-usage.json" \
  --baseline scripts/firmware-stack-baseline.json
xtensa-esp32s3-elf-nm --defined-only "$work/build/tesla-key-esp32.elf" > "$proof/symbols.txt"
python3 - "$proof/symbols.txt" <<'PY'
from pathlib import Path
import sys
symbols = Path(sys.argv[1]).read_text()
assert 'led_task' in symbols, 'the real LED task is absent from the linked image'
PY
cp "$work/sdkconfig" "$proof/sdkconfig"
sha256sum "$work/build/tesla-key-esp32.bin" "$work/build/tesla-key-esp32.elf" > "$proof/image-hashes.txt"
echo "optional LED SDK variant: PASS (ESP32-S3, enabled task, original stack/layout/image gates; $proof)"
