#!/usr/bin/env bash
# Claude compatibility adapter; canonical implementation is runner-neutral and report-only.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$root/tools/agent-hooks/agent_hook.py" build-efficiency "$@"
