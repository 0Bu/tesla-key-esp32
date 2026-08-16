#!/usr/bin/env bash
# Claude compatibility alias for the consolidated runner-neutral pre-tool guards.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$root/tools/agent-hooks/agent_hook.py" pre-tool-guards --runner claude
