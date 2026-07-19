#!/usr/bin/env bash
# Stop gate: before a Claude Code session may finish a turn, run the host-side mock
# build (scripts/run-mock-tests.sh) whenever the working tree has uncommitted changes
# under main/ or test/. If the pure-logic tests fail, the turn is blocked (exit 2) and
# the failing output is fed back so Claude fixes it *before* claiming done — instead of
# pushing red and waiting on the slow CI `logic-test` job.
#
# Why a Stop hook (not PostToolUse): PostToolUse fires after every edit, when a
# mid-refactor tree may not even compile — that produces false failures and noise. Stop
# fires once, when Claude thinks it is finished: the right single gate point.
#
# Scope: only the in-session, PRE-COMMIT window. Once changes are committed the tree is
# clean here and the CI `logic-test` job (which the firmware build depends on) takes over.
# So this and CI are complementary, never redundant.
#
# Graceful degradation:
#   • No host toolchain (cmake + a C++ compiler)  -> allow (a board-only host can't run it).
#   • No relevant uncommitted changes             -> allow (don't run on doc/Q&A turns).
#   • Tests fail but the working tree is unchanged since the last block -> allow, with a
#     note: the agent made no progress, so stop looping and defer to the human / CI rather
#     than spinning forever. (Blocks once per distinct working-tree state.)
#
# Exit codes: 0 = allow the turn to end, 2 = block it (stderr is fed back to Claude).

proj="${CLAUDE_PROJECT_DIR:-$PWD}"
cd "$proj" 2>/dev/null || exit 0

# ── Toolchain present? Otherwise this host simply can't run the mock build. ──────────
command -v cmake >/dev/null 2>&1 || exit 0
command -v g++ >/dev/null 2>&1 || command -v c++ >/dev/null 2>&1 || \
    command -v clang++ >/dev/null 2>&1 || exit 0

# ── Anything to verify? Only gate when uncommitted changes touch main/ or test/. ─────
# (Committed changes are the CI job's responsibility; here the tree would be clean.)
changes="$(git status --porcelain -- main test 2>/dev/null)"
[ -n "$changes" ] || exit 0

# ── Run the mock build. ──────────────────────────────────────────────────────────────
out="$(./scripts/run-mock-tests.sh 2>&1)"
rc=$?

marker="$proj/.claude/.logic-tests-blocked"

if [ "$rc" -eq 0 ]; then
    rm -f "$marker"
    exit 0   # green -> allow the turn to end
fi

# Tests failed. Signature of the exact working-tree state we are blocking on, so a repeat
# stop with no edits in between doesn't loop forever.
sig="$( { git status --porcelain -- main test; git diff -- main test; } 2>/dev/null \
        | sha1sum | cut -d' ' -f1)"
if [ -f "$marker" ] && [ "$(cat "$marker" 2>/dev/null)" = "$sig" ]; then
    {
        echo "NOTE: host logic tests are still failing, but nothing changed since the last"
        echo "run — not blocking again to avoid a loop. Fix scripts/run-mock-tests.sh"
        echo "failures before merging (the CI \`logic-test\` job gates the build regardless)."
    } >&2
    exit 0
fi
printf '%s\n' "$sig" > "$marker"

# Block: feed the failure back with exactly what to do.
{
    echo "BLOCKED: host-side logic tests (scripts/run-mock-tests.sh) are failing."
    echo "Fix them before finishing — do not push red (the CI \`logic-test\` job gates the build)."
    echo
    echo "----- mock build output (tail) -----"
    printf '%s\n' "$out" | tail -n 30
    echo "------------------------------------"
    echo
    echo "Pure logic lives in main/logic/ and its checks in test/test_logic.cpp."
    echo "Re-run locally with:  scripts/run-mock-tests.sh"
} >&2
exit 2
