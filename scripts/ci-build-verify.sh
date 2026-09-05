#!/usr/bin/env bash
# Complete CI contract: unsigned four-target build, disposable-key release path, then one clean
# reproducibility rebuild for all four targets. The real signing key is never available here.
set -euo pipefail

target_override=""
verify_only=0
if [[ "${1:-}" == --target ]]; then
  shift
  target_override="${1:?usage: ci-build-verify.sh [--target <target>|--verify-only] <display-version> <source-sha>}"
  shift
  case "$target_override" in
    esp32|esp32s3|esp32c3|esp32c6) ;;
    *) echo "unsupported target: $target_override" >&2; exit 2 ;;
  esac
elif [[ "${1:-}" == --verify-only ]]; then
  shift
  verify_only=1
fi

version="${1:?usage: ci-build-verify.sh <display-version> <source-sha>}"
source_sha="${2:?usage: ci-build-verify.sh <display-version> <source-sha>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

# These tests compile against the exact cJSON sources under the pinned ESP-IDF's IDF_PATH. Run
# their sanitizer modes once before spending time on eight target builds (four primary + four
# isolated reproducibility rebuilds).
if [[ -z "$target_override" ]]; then
  CJSON_OOM_SANITIZE=1 bash ./test/run-cjson-oom-tests.sh
  MQTT_JSON_SANITIZE=1 bash ./test/run-mqtt-json-publish-tests.sh
fi

if [[ "$verify_only" -eq 0 ]]; then
  if [[ -n "$target_override" ]]; then
    ./scripts/ci-build-all.sh --target "$target_override" "$version" "$source_sha"
  else
    ./scripts/ci-build-all.sh "$version" "$source_sha"
  fi
fi

if [[ "$verify_only" -eq 1 ]]; then
  {
    printf 'head_sha=%s\n' "$source_sha"
    printf 'display_version=%s\n' "$version"
  } > dist/build-metadata.txt
  inventory_source_sha="$source_sha"
  if [[ "$inventory_source_sha" == local ]]; then
    inventory_source_sha="$(git -c core.fsmonitor=false rev-parse HEAD)"
  fi
  python3 ./scripts/check-build-artifact-inventory.py \
    --write --artifact-root . --source-root . \
    --expected-source-sha "$inventory_source_sha" --version "$version"
fi

if [[ -z "$target_override" ]]; then
  python3 ./scripts/check-build-artifact-inventory.py \
    --verify --artifact-root . --source-root . \
    --expected-source-sha "$source_sha" --version "$version"
  ./scripts/test-release-contract.sh "$version" "$source_sha" _unsigned
fi

if [[ "$verify_only" -eq 1 || "${CI_SKIP_REPRO:-0}" == 1 ]]; then
  exit 0
fi

if [[ -z "$target_override" ]]; then
# EXACT_FOUR_TARGETS_BEGIN repro
for target in esp32 esp32s3 esp32c3 esp32c6; do
  ./scripts/check-reproducible-build.sh "$target" \
    "$version" \
    "_unsigned/$target/tesla-key-esp32.bin" \
    "dist/$target/tesla-key-esp32-$target.elf"
done
# EXACT_FOUR_TARGETS_END repro
else
  ./scripts/check-reproducible-build.sh "$target_override" \
    "$version" \
    "_unsigned/$target_override/tesla-key-esp32.bin" \
    "dist/$target_override/tesla-key-esp32-$target_override.elf"
fi
