#!/usr/bin/env bash
# Claude compatibility adapter for changed host logic tests.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$root/tools/agent-hooks/agent_hook.py" stop-logic-tests
