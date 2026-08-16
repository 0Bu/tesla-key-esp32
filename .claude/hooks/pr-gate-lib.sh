#!/usr/bin/env bash
# Claude compatibility adapter; source the runner-neutral PR-gate library.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=/dev/null
. "$root/tools/agent-hooks/pr-gate-lib.sh"
