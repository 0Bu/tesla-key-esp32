#!/usr/bin/env bash
# Discover and, under a caller-provided per-PR concurrency lock, remove stale gh-pages previews.
# A preview is current only while the PR is open, same-repository, labelled signed-preview and its
# schema-v2 manifest sourceSha equals the PR's current head.  API failures abort; they are never
# interpreted as permission to delete.
set -euo pipefail

eligible_json() {
  local pr_json="$1" manifest_json="$2" repo="$3" head
  head="$(printf '%s' "$pr_json" | jq -er '.head.sha')" || return 1
  printf '%s' "$pr_json" | jq -e --arg repo "$repo" '
    .state == "open"
    and .head.repo.full_name == $repo
    and any(.labels[]?; .name == "signed-preview")
  ' >/dev/null || return 1
  printf '%s' "$manifest_json" | jq -e --arg head "$head" '
    .layoutVersion == 2 and .sourceSha == $head
  ' >/dev/null
}

self_test() {
  local repo=owner/project sha=0123456789abcdef0123456789abcdef01234567
  local pr manifest
  pr="$(printf '{"state":"open","head":{"sha":"%s","repo":{"full_name":"%s"}},"labels":[{"name":"signed-preview"}]}' "$sha" "$repo")"
  manifest="$(printf '{"layoutVersion":2,"sourceSha":"%s"}' "$sha")"
  eligible_json "$pr" "$manifest" "$repo" || { echo "eligible preview rejected" >&2; exit 1; }
  ! eligible_json "${pr/\"open\"/\"closed\"}" "$manifest" "$repo" || {
    echo "closed preview accepted" >&2; exit 1;
  }
  ! eligible_json "$pr" '{"layoutVersion":2,"sourceSha":"ffffffffffffffffffffffffffffffffffffffff"}' "$repo" || {
    echo "stale-head preview accepted" >&2; exit 1;
  }
  ! eligible_json "${pr/owner\/project/fork\/project}" "$manifest" "$repo" || {
    echo "fork preview accepted" >&2; exit 1;
  }
  echo "PR-preview reconciliation self-test: PASS"
}

[[ "${1:-}" == --self-test ]] && { self_test; exit 0; }

mode="${1:?usage: reconcile-pr-previews.sh list | remove-if-stale <PR> | --self-test}"
: "${GH_TOKEN:?GH_TOKEN required}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY required}"
command -v gh >/dev/null 2>&1 || { echo "gh CLI is required" >&2; exit 1; }
command -v jq >/dev/null 2>&1 || { echo "jq is required" >&2; exit 1; }

load_tree() {
  local tree
  tree="$(gh api "repos/$GITHUB_REPOSITORY/git/trees/gh-pages?recursive=1")" || return 1
  printf '%s' "$tree" | jq -e '.truncated == false' >/dev/null || {
    echo "gh-pages tree response is truncated; refusing partial reconciliation" >&2; return 1;
  }
  printf '%s' "$tree"
}

preview_numbers() {
  printf '%s' "$1" | jq -r '.tree[]?.path' \
    | sed -nE 's#^PR/([0-9]+)(/.*)?$#\1#p' | LC_ALL=C sort -nu
}

manifest_for() {
  local tree="$1" number="$2" path="PR/$2/manifest.json" matches blob
  matches="$(printf '%s' "$tree" | jq -r --arg path "$path" \
    '.tree[]? | select(.path == $path and .type == "blob") | .sha')"
  if [[ -z "$matches" ]]; then
    printf '{}'
    return 0
  fi
  [[ "$(printf '%s\n' "$matches" | awk 'NF {n++} END {print n+0}')" == 1 ]] || {
    echo "multiple manifest blobs found for PR $number" >&2; return 1;
  }
  blob="$(gh api "repos/$GITHUB_REPOSITORY/git/blobs/$matches")" || return 1
  printf '%s' "$blob" | jq -er '
    select(.encoding == "base64") | .content | gsub("\\n"; "") | @base64d
  ' | jq -c .
}

current_eligibility() {
  local number="$1" tree="$2" pr_json manifest_json
  pr_json="$(gh api "repos/$GITHUB_REPOSITORY/pulls/$number")" || return 2
  manifest_json="$(manifest_for "$tree" "$number")" || return 2
  eligible_json "$pr_json" "$manifest_json" "$GITHUB_REPOSITORY"
}

case "$mode" in
  list)
    tree="$(load_tree)"
    stale=""
    while read -r number; do
      [[ -z "$number" ]] && continue
      if current_eligibility "$number" "$tree"; then
        echo "keeping eligible PR $number preview" >&2
      else
        status=$?
        [[ "$status" == 1 ]] || { echo "could not validate PR $number" >&2; exit "$status"; }
        stale="${stale}${number}"$'\n'
      fi
    done <<< "$(preview_numbers "$tree")"
    printf '%s' "$stale" | jq -Rsc 'split("\n") | map(select(length > 0) | tonumber)'
    ;;
  remove-if-stale)
    number="${2:?remove-if-stale needs a PR number}"
    [[ "$number" =~ ^[0-9]+$ ]] || { echo "PR number must be digits" >&2; exit 2; }
    tree="$(load_tree)"
    if ! preview_numbers "$tree" | grep -qx "$number"; then
      echo "PR $number preview is already absent"
      exit 0
    fi
    if current_eligibility "$number" "$tree"; then
      echo "PR $number preview became eligible/current; keeping it"
      exit 0
    else
      status=$?
      [[ "$status" == 1 ]] || { echo "could not revalidate PR $number" >&2; exit "$status"; }
    fi
    : "${GITHUB_TOKEN:?GITHUB_TOKEN required for removal}"
    "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)/publish-pages-branch.sh" rm "$number"
    ;;
  *) echo "unknown mode: $mode" >&2; exit 2 ;;
esac
