#!/usr/bin/env bash
# Compatibility entrypoint for the runner-neutral aggregate PR-gate adversarial suite.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
"$root/scripts/stamp-pr-gates.sh" --self-test
exec "$root/tools/agent-hooks/selftest.sh"
