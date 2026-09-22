#!/usr/bin/env bash
# Local firmware size and baseline verification script.
# Builds target(s) inside the pinned ESP-IDF Docker container and validates against
# hard partition limits and scripts/firmware-size-baseline.json.
#
# Usage:
#   scripts/check-firmware-size.sh [--target <target>] [--update-baseline]
#   scripts/check-firmware-size.sh --all [--update-baseline]
#   scripts/check-firmware-size.sh --self-test
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

TARGETS_DEFAULT=("esp32c6")
ALL_TARGETS=("esp32" "esp32s3" "esp32c3" "esp32c6")
APP_POLICY_LIMIT=$((0x1e8000)) # 1,998,848 B (32 KiB below 0x1f0000)
C6_64K_CLIFF=$((1966080))      # 0x1e0000 boundary for Secure Boot v2 projection

target_list=()
update_baseline=0
self_test=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target)
      shift
      [[ $# -gt 0 ]] || { echo "ERROR: --target requires an argument" >&2; exit 2; }
      target_list+=("$1")
      shift
      ;;
    --all)
      target_list=("${ALL_TARGETS[@]}")
      shift
      ;;
    --update-baseline)
      update_baseline=1
      shift
      ;;
    --self-test)
      self_test=1
      shift
      ;;
    -h|--help)
      cat <<HELP
Usage: $(basename "$0") [options]

Options:
  --target <chip>    Target chip (esp32, esp32s3, esp32c3, esp32c6). Default: esp32c6
  --all              Check all four supported targets
  --update-baseline  Update scripts/firmware-size-baseline.json if build passes policy
  --self-test        Run argument and self-validation tests
  -h, --help         Show this help message
HELP
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ "$self_test" -eq 1 ]]; then
  echo "check-firmware-size: running self-test..."
  python3 scripts/report-firmware-size.py --self-test
  echo "check-firmware-size: self-test PASS"
  exit 0
fi

if [[ ${#target_list[@]} -eq 0 ]]; then
  target_list=("${TARGETS_DEFAULT[@]}")
fi

# Ensure docker is available
if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
  echo "ERROR: Docker is not running or not accessible. An ESP-IDF build requires Docker." >&2
  exit 1
fi

echo "==> Verifying firmware size for target(s): ${target_list[*]}"

overall_failed=0

for target in "${target_list[@]}"; do
  case "$target" in
    esp32|esp32s3|esp32c3|esp32c6) ;;
    *) echo "ERROR: unsupported target '$target'" >&2; exit 2 ;;
  esac

  echo ""
  echo "--- Building and measuring $target ---"
  build_flags=(--target "$target")
  if [[ "$update_baseline" -eq 1 ]]; then
    build_flags+=(--no-enforce-budget)
  fi
  ./scripts/idf-docker.sh ./scripts/ci-build-all.sh "${build_flags[@]}" local local

  unsigned_bin="_unsigned/$target/tesla-key-esp32.bin"
  size_json="dist/$target/size-$target.json"
  projected_size_file="dist/$target/projected-signed-size.txt"

  if [[ ! -f "$unsigned_bin" || ! -f "$size_json" || ! -f "$projected_size_file" ]]; then
    echo "ERROR: build artifacts missing for $target" >&2
    overall_failed=1
    continue
  fi

  unsigned_size=$(wc -c < "$unsigned_bin" | tr -d '[:space:]')
  projected_signed_size=$(tr -d '[:space:]' < "$projected_size_file")

  echo "Target: $target"
  echo "  Unsigned app binary:      $unsigned_size B"
  echo "  Projected signed app:     $projected_signed_size B (Policy limit: $APP_POLICY_LIMIT B)"

  if (( projected_signed_size > APP_POLICY_LIMIT )); then
    echo "❌ ERROR: $target exceeds policy limit $APP_POLICY_LIMIT B!" >&2
    overall_failed=1
  fi

  if [[ "$target" == "esp32c6" ]] && (( unsigned_size > C6_64K_CLIFF )); then
    echo "❌ CRITICAL: esp32c6 unsigned app exceeds 64 KiB quantization boundary ($C6_64K_CLIFF B)!" >&2
    echo "   Signed app will overflow the 0x1f0000 OTA flash partition!" >&2
    overall_failed=1
  fi

  # Run size reporter
  if [[ "$update_baseline" -eq 1 ]]; then
    python3 - "$target" "$size_json" "$unsigned_size" "$repo_root/scripts/firmware-size-baseline.json" <<'PY'
import json, sys
target = sys.argv[1]
size_json = sys.argv[2]
unsigned_size = int(sys.argv[3])
baseline_path = sys.argv[4]

with open(size_json, "r", encoding="utf-8") as f:
    observed = json.load(f)

with open(baseline_path, "r", encoding="utf-8") as f:
    baseline = json.load(f)

tgt_budget = baseline["targets"][target]
flash_code = observed.get("flash_code", 0)
flash_rodata = observed.get("flash_rodata", 0)
flash_code_rodata = flash_code + flash_rodata
total_size = observed.get("total_size", 0)

tgt_budget["maxUnsignedApp"] = max(tgt_budget["maxUnsignedApp"], unsigned_size)
tgt_budget["maxElfTotal"] = max(tgt_budget["maxElfTotal"], total_size)
tgt_budget["maxFlashCodeAndRodata"] = max(tgt_budget["maxFlashCodeAndRodata"], flash_code_rodata)

if tgt_budget["memoryModel"] == "unified":
    tgt_budget["maxStaticUsed"] = max(tgt_budget["maxStaticUsed"], observed.get("used_diram", 0))
    tgt_budget["maxBss"] = max(tgt_budget["maxBss"], observed.get("diram_bss", 0))
else:
    tgt_budget["maxStaticUsed"] = max(tgt_budget["maxStaticUsed"], observed.get("used_dram", 0))
    tgt_budget["maxBss"] = max(tgt_budget["maxBss"], observed.get("dram_bss", 0))
    tgt_budget["maxIramUsed"] = max(tgt_budget.get("maxIramUsed", 0), observed.get("used_iram", 0))

with open(baseline_path, "w", encoding="utf-8") as f:
    json.dump(baseline, f, indent=2)
    f.write("\n")

print(f"Updated baseline for {target}: maxElfTotal={tgt_budget['maxElfTotal']} maxFlashCodeAndRodata={tgt_budget['maxFlashCodeAndRodata']}")
PY
  fi

  # Check budget against baseline
  if ! python3 scripts/report-firmware-size.py \
      --idf-size "$size_json" \
      --unsigned-app "$unsigned_bin" \
      --projected-signed-size "$projected_signed_size" \
      --policy-limit "$APP_POLICY_LIMIT" \
      --target "$target" \
      --budget-baseline scripts/firmware-size-baseline.json \
      --enforce-budget; then
    echo "❌ ERROR: $target violated reviewed firmware size baseline!" >&2
    echo "   If this growth is intentional and reviewed, rerun with --update-baseline." >&2
    overall_failed=1
  else
    echo "✅ $target firmware size is within reviewed baseline and policy limits."
  fi
done

if [[ "$overall_failed" -ne 0 ]]; then
  echo ""
  echo "❌ Firmware size check FAILED." >&2
  exit 1
fi

echo ""
echo "✅ All checked targets passed firmware size validation."
exit 0
