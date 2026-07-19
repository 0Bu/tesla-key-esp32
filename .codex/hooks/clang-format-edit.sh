#!/usr/bin/env bash
# PostToolUse hook: after Claude edits a C/C++ source file under main/ or test/, run
# `clang-format -i` on THAT ONE FILE so style stays uniform with the checked-in
# .clang-format. Deliberately bounded:
#
#   • Single-file blast radius — it only touches the file just edited, never the tree.
#   • SILENT NO-OP wherever clang-format is absent (this dev box AND CI both lack it),
#     so an unverified style spec can never reflow code in an environment that can't
#     review the result. It starts acting only once someone installs clang-format.
#
# ONE-TIME before trusting auto-format, verify churn on a host WITH clang-format:
#     clang-format --dry-run -Werror main/*.cpp main/*.hpp main/logic/*.hpp test/*.cpp
# If it rewrites large hand-aligned blocks, tune .clang-format (especially
# AllowShortFunctionsOnASingleLine) until the dry-run is quiet, THEN rely on this hook.
# To make the hook check-only (report, never rewrite), swap the `-i` line for the
# `--dry-run -Werror` line marked below.
#
# Never blocks: PostToolUse is advisory, so this always exits 0.

set -uo pipefail

# Nothing to do (and nothing risky) unless clang-format is actually installed.
command -v clang-format >/dev/null 2>&1 || exit 0
command -v jq           >/dev/null 2>&1 || exit 0

input="$(cat 2>/dev/null)"
file="$(printf '%s' "$input" | jq -r '.tool_input.file_path // ""' 2>/dev/null)"
[ -n "$file" ] && [ -f "$file" ] || exit 0

# Never touch vendored or generated trees.
case "$file" in
  */managed_components/*|*/build/*|*/build_mock/*|*/_site/*) exit 0 ;;
esac
# Only our own first-party C/C++ (main/ or test/).
case "$file" in
  */main/*|*/test/*) ;;
  *) exit 0 ;;
esac
case "$file" in
  *.c|*.cc|*.cpp|*.cxx|*.h|*.hpp|*.hxx) ;;
  *) exit 0 ;;
esac

clang-format -i -- "$file" 2>/dev/null || true
# Check-only alternative (no rewrite) — comment the line above, uncomment this one:
# clang-format --dry-run -Werror -- "$file" 1>&2 || true
exit 0
