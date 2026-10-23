#!/usr/bin/env bash
# Canonical CMake entry point; native harnesses may pass an explicit component path, never patches.
set -euo pipefail
export LC_ALL=C
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
exec python3 "$repo_root/scripts/apply-tesla-ble-patches.py" "$@"
