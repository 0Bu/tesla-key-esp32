#!/usr/bin/env bash
# Complete CI contract: unsigned four-target build, disposable-key release path, then one clean
# reproducibility rebuild for all four targets. The real signing key is never available here.
set -euo pipefail

version="${1:?usage: ci-build-verify.sh <display-version> <source-sha>}"
source_sha="${2:?usage: ci-build-verify.sh <display-version> <source-sha>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

# These tests compile against the exact cJSON sources under the pinned ESP-IDF's IDF_PATH. Run
# their sanitizer modes once before spending time on eight target builds (four primary + four
# isolated reproducibility rebuilds).
CJSON_OOM_SANITIZE=1 bash ./test/run-cjson-oom-tests.sh
MQTT_JSON_SANITIZE=1 bash ./test/run-mqtt-json-publish-tests.sh

./scripts/ci-build-all.sh "$version" "$source_sha"
python3 ./scripts/check-build-artifact-inventory.py \
  --verify --artifact-root . --source-root . \
  --expected-source-sha "$source_sha" --version "$version"
./scripts/test-release-contract.sh "$version" "$source_sha" _unsigned
# EXACT_FOUR_TARGETS_BEGIN repro
for target in esp32 esp32s3 esp32c3 esp32c6; do
  ./scripts/check-reproducible-build.sh "$target" \
    "_unsigned/$target/tesla-key-esp32.bin" \
    "dist/$target/tesla-key-esp32-$target.elf"
done
# EXACT_FOUR_TARGETS_END repro
