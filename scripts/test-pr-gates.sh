#!/usr/bin/env bash
# Compatibility entrypoint for the runner-neutral aggregate PR-gate adversarial suite.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$root/tools/agent-hooks/selftest.sh"
