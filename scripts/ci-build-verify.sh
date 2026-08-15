#!/usr/bin/env bash
# Complete CI contract: unsigned four-target build, disposable-key release path, then one clean
# reproducibility double-build.  The real signing key is never available in this job.
set -euo pipefail

version="${1:?usage: ci-build-verify.sh <display-version> <source-sha>}"
source_sha="${2:?usage: ci-build-verify.sh <display-version> <source-sha>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo_root"

./scripts/ci-build-all.sh "$version" "$source_sha"
./scripts/test-release-contract.sh "$version" "$source_sha" _unsigned
# esp32 is the binding target for the deterministic-output contract; the four-target build above
# still validates every target's defaults, dependency lock and projected signed size.
./scripts/check-reproducible-build.sh esp32
