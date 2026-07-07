#!/usr/bin/env bash
# Publish a built site directory into the repo's `gh-pages` branch — the same-origin host
# for the ESP Web Tools installer (root site) and the per-PR preview installers (PR/<N>/).
#
# Why a branch (not the Actions Pages artifact): the browser flasher fetches the manifest and
# every .bin in-page, and GitHub release assets carry no CORS headers, so the parts must be
# same-origin with the installer. The Actions artifact deploy replaces the WHOLE site atomically
# and only from main, so it can't host per-PR subpaths. A gh-pages branch lets main own the root
# and each PR own PR/<N>/ independently. See docs/ARCHITECTURE.md (PR preview installer).
#
#   root <srcdir>                    sync <srcdir> into the gh-pages ROOT, preserving the
#                                    PR/ preview tree and previews.json
#   pr   <srcdir> <N> [title] [ver]  replace gh-pages PR/<N>/ with <srcdir>, and add/replace
#                                    the PR's entry in previews.json (N = PR number, digits)
#   rm   <N>                         remove gh-pages PR/<N>/ and its previews.json entry
#
# previews.json (gh-pages root) is the index the installer's firmware picker reads: one entry
# {pr,title,version,path} per open PR that currently has a preview. Needs `jq` (present on the
# GitHub runner).
#
# Idempotent and concurrency-safe: main, PR and cleanup runs can all push gh-pages at once, so
# each attempt re-clones the LATEST gh-pages and re-applies the change on top → always a
# fast-forward push, no binary-merge conflicts.
#
# Env (CI): GITHUB_TOKEN (contents:write), GITHUB_REPOSITORY, optionally GITHUB_SERVER_URL.
set -euo pipefail

mode="${1:?usage: publish-pages-branch.sh root <srcdir> | pr <srcdir> <N> | rm <N>}"
: "${GITHUB_TOKEN:?GITHUB_TOKEN required}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY required}"

server="${GITHUB_SERVER_URL:-https://github.com}"
remote="https://x-access-token:${GITHUB_TOKEN}@${server#https://}/${GITHUB_REPOSITORY}.git"

# Parse + validate args per mode up front (fail fast, before touching the remote).
src=""; num=""; title=""; version=""
case "$mode" in
  root) src="${2:?root needs <srcdir>}" ;;
  pr)   src="${2:?pr needs <srcdir>}"; num="${3:?pr needs <N>}"; title="${4:-}"; version="${5:-}" ;;
  rm)   num="${2:?rm needs <N>}" ;;
  *)    echo "unknown mode '$mode'" >&2; exit 1 ;;
esac
if [ -n "$num" ] && ! [[ "$num" =~ ^[0-9]+$ ]]; then
  echo "PR number must be digits, got '$num'" >&2; exit 1
fi
[ -n "$src" ] && [ ! -d "$src" ] && { echo "source dir '$src' not found" >&2; exit 1; }

# Add/replace this PR's entry in the root previews.json (the installer's picker index). Each run
# touches only its OWN entry, so concurrent PRs merge under the re-clone-and-reapply loop below.
update_index() {
  local idx="$1/previews.json" cur='[]'
  [ -f "$idx" ] && cur="$(cat "$idx")"
  jq -n --argjson cur "$cur" --argjson pr "$num" --arg title "$title" --arg version "$version" \
    '$cur | map(select(.pr != $pr))
          + [{pr:$pr, title:$title, version:$version, path:"PR/\($pr)/"}]
          | sort_by(.pr) | reverse' > "$idx"
}
remove_from_index() {
  local idx="$1/previews.json"
  [ -f "$idx" ] || return 0
  jq --argjson pr "$num" 'map(select(.pr != $pr))' "$idx" > "$idx.tmp" && mv "$idx.tmp" "$idx"
}

# Apply the requested change to a fresh gh-pages checkout at $1 (idempotent).
apply_changes() {
  local work="$1"
  case "$mode" in
    root)
      # Root files only — NEVER delete the PR/ preview tree or previews.json (PR-owned).
      rsync -a --delete --exclude='.git/' --exclude='PR/' --exclude='previews.json' "$src"/ "$work"/
      ;;
    pr)
      rm -rf "${work:?}/PR/$num"
      mkdir -p "$work/PR/$num"
      rsync -a --delete --exclude='.git/' "$src"/ "$work/PR/$num"/
      update_index "$work"
      ;;
    rm)
      rm -rf "${work:?}/PR/$num"
      remove_from_index "$work"
      ;;
  esac
}

commit_msg() {
  case "$mode" in
    root) echo "pages: publish site (root)" ;;
    pr)   echo "pages: publish PR $num preview" ;;
    rm)   echo "pages: remove PR $num preview" ;;
  esac
}

for attempt in 1 2 3 4 5; do
  work="$(mktemp -d)"
  # Clone gh-pages if it exists, else start it as an orphan (first-ever publish).
  if git clone --quiet --depth 1 --branch gh-pages "$remote" "$work" 2>/dev/null; then :; else
    git clone --quiet --depth 1 "$remote" "$work"
    git -C "$work" checkout --orphan gh-pages
    git -C "$work" rm -rfq . >/dev/null 2>&1 || true
  fi
  git -C "$work" config user.name  "github-actions[bot]"
  git -C "$work" config user.email "41898282+github-actions[bot]@users.noreply.github.com"

  apply_changes "$work"

  git -C "$work" add -A
  if git -C "$work" diff --cached --quiet; then
    echo "gh-pages: nothing to change ($(commit_msg))"
    rm -rf "$work"; exit 0
  fi
  git -C "$work" commit --quiet -m "$(commit_msg)"

  if git -C "$work" push --quiet "$remote" HEAD:gh-pages 2>/dev/null; then
    echo "gh-pages: $(commit_msg) — pushed (attempt $attempt)"
    rm -rf "$work"; exit 0
  fi
  echo "gh-pages: push rejected, retrying with a fresh clone (attempt $attempt)…" >&2
  rm -rf "$work"
  sleep $((attempt * 3))
done

echo "ERROR: could not push gh-pages after 5 attempts" >&2
exit 1
