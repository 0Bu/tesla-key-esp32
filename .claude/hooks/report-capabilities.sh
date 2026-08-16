#!/usr/bin/env bash
# Claude compatibility adapter for runner-neutral capability discovery.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$root/tools/agent-hooks/agent_hook.py" capabilities
