#!/usr/bin/env bash
# Claude compatibility adapter; the aggregate core enforces create/push skill-audit evidence.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "$root/tools/agent-hooks/require-pr-gates.sh" --project-dir "$root"
