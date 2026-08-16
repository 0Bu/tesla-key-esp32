#!/usr/bin/env bash
# Claude compatibility adapter for the aggregate runner-neutral PR policy.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "$root/tools/agent-hooks/require-pr-gates.sh" --project-dir "$root"
