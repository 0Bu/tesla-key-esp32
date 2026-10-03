#!/usr/bin/env bash
# Publish a built site directory into the repo's `gh-pages` branch — the same-origin host
# for the inline Web Serial installer (root site) and the per-PR preview installers (PR/<N>/).
#
# Why a branch (not the Actions Pages artifact): the browser flasher fetches the manifest and
# every .bin in-page, and GitHub release assets carry no CORS headers, so the parts must be
# same-origin with the installer. The Actions artifact deploy replaces the WHOLE site atomically
# and only from main, so it can't host per-PR subpaths. A gh-pages branch lets main own the root
# and each PR own PR/<N>/ independently. See docs/ARCHITECTURE.md (PR preview installer).
#
# The root installer always flashes main; a PR's firmware is reached only by its own page at
# PR/<N>/ (open the URL directly, or from the link on the PR) — there is no root version picker,
# so no cross-PR index file is maintained.
#
#   root <srcdir>       sync <srcdir> into the gh-pages ROOT, preserving the PR/ preview tree and dev/
#   dev  <srcdir>       sync <srcdir> into gh-pages dev/, preserving root and PR/
#   pr   <srcdir> <N>   replace gh-pages PR/<N>/ with <srcdir>   (N = PR number, digits)
#   rm   <N>            remove gh-pages PR/<N>/
#
# Idempotent and concurrency-safe: main, PR and cleanup runs can all push gh-pages at once, so
# each attempt re-clones the LATEST gh-pages and re-applies the change on top → always a
# fast-forward push, no binary-merge conflicts.
#
# Env (CI): GITHUB_TOKEN (contents:write), GITHUB_REPOSITORY, optionally GITHUB_SERVER_URL.
# Dev publication also requires SOURCE_SHA, bound to the manifest and current remote main
# immediately before each push (including retries) or unchanged-byte build request.
set -euo pipefail

trigger_pages_build() {
  if ! command -v gh >/dev/null 2>&1; then
    echo "ERROR: gh CLI is required to trigger GitHub Pages build" >&2
    return 1
  fi
  local token="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
  local err=""
  for pages_attempt in 1 2 3; do
    if err="$(GH_TOKEN="$token" gh api --method POST "repos/$GITHUB_REPOSITORY/pages/builds" 2>&1 >/dev/null)"; then
      return 0
    fi
    echo "gh-pages: failed to trigger pages build (attempt $pages_attempt/3): $err" >&2
    if [ -z "${TEST_REMOTE:-}" ]; then
      sleep $((pages_attempt * 2))
    fi
  done
  echo "ERROR: failed to trigger pages build via GitHub API after 3 attempts" >&2
  return 1
}

self_test() {
  local tmp
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT

  local fake_bin="$tmp/bin"
  local fake_gh="$fake_bin/gh"
  mkdir -p "$fake_bin"

  # 1. Missing gh fails
  (
    export PATH="$fake_bin"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    if trigger_pages_build 2>/dev/null; then
      echo "self_test: trigger_pages_build unexpectedly succeeded without gh" >&2
      exit 1
    fi
  ) || exit 1

  # 2. Failing gh fails after 3 attempts
  cat <<'EOF' > "$fake_gh"
#!/usr/bin/env bash
if [ "$1" = "api" ] && [ "$2" = "--method" ] && [ "$3" = "POST" ]; then
  echo "attempt" >> "$TEST_TMP/gh_calls"
  exit 1
fi
exit 0
EOF
  chmod +x "$fake_gh"

  (
    export PATH="$fake_bin:$PATH"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    export TEST_TMP="$tmp"
    export TEST_REMOTE="dummy"
    if trigger_pages_build 2>/dev/null; then
      echo "self_test: trigger_pages_build unexpectedly succeeded with failing gh" >&2
      exit 1
    fi
    local calls
    calls="$(wc -l < "$tmp/gh_calls" | tr -d ' ')"
    if [ "$calls" -ne 3 ]; then
      echo "self_test: expected 3 attempts, got $calls" >&2
      exit 1
    fi
  ) || exit 1

  # 3. Successful gh succeeds
  cat <<'EOF' > "$fake_gh"
#!/usr/bin/env bash
if [ "$1" = "api" ] && [ "$2" = "--method" ] && [ "$3" = "POST" ]; then
  echo "ok" >> "$TEST_TMP/gh_ok_calls"
  exit 0
fi
exit 0
EOF
  chmod +x "$fake_gh"

  (
    export PATH="$fake_bin:$PATH"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    export TEST_TMP="$tmp"
    export TEST_REMOTE="dummy"
    if ! trigger_pages_build 2>/dev/null; then
      echo "self_test: trigger_pages_build failed with working gh" >&2
      exit 1
    fi
    local calls
    calls="$(wc -l < "$tmp/gh_ok_calls" | tr -d ' ')"
    if [ "$calls" -ne 1 ]; then
      echo "self_test: expected 1 call, got $calls" >&2
      exit 1
    fi
  ) || exit 1

  # 4. Full publication and unchanged-byte retry
  local bare_repo="$tmp/remote.git"
  git init --bare --quiet "$bare_repo"
  local src="$tmp/site"
  mkdir -p "$src"
  echo "hello pages" > "$src/index.html"

  # Initial publish with working gh
  (
    export PATH="$fake_bin:$PATH"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    export TEST_REMOTE="$bare_repo"
    export TEST_TMP="$tmp"
    "$0" root "$src" >/dev/null 2>&1
  ) || exit 1

  # Retry with identical bytes and failing gh must fail
  cat <<'EOF' > "$fake_gh"
#!/usr/bin/env bash
if [ "$1" = "api" ] && [ "$2" = "--method" ] && [ "$3" = "POST" ]; then
  exit 1
fi
exit 0
EOF
  chmod +x "$fake_gh"

  (
    export PATH="$fake_bin:$PATH"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    export TEST_REMOTE="$bare_repo"
    export TEST_TMP="$tmp"
    if "$0" root "$src" >/dev/null 2>&1; then
      echo "self_test: unchanged-byte retry unexpectedly succeeded when gh build trigger failed" >&2
      exit 1
    fi
  ) || exit 1

  # Retry with identical bytes and working gh must succeed
  cat <<'EOF' > "$fake_gh"
#!/usr/bin/env bash
if [ "$1" = "api" ] && [ "$2" = "--method" ] && [ "$3" = "POST" ]; then
  echo "unchanged_ok" >> "$TEST_TMP/unchanged_calls"
  exit 0
fi
exit 0
EOF
  chmod +x "$fake_gh"

  (
    export PATH="$fake_bin:$PATH"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    export TEST_REMOTE="$bare_repo"
    export TEST_TMP="$tmp"
    if ! "$0" root "$src" >/dev/null 2>&1; then
      echo "self_test: unchanged-byte retry failed with working gh" >&2
      exit 1
    fi
    local calls
    calls="$(wc -l < "$tmp/unchanged_calls" | tr -d ' ')"
    if [ "$calls" -ne 1 ]; then
      echo "self_test: expected 1 build trigger for unchanged bytes, got $calls" >&2
      exit 1
    fi
  ) || exit 1

  # 5. Subshell error propagation: verify that subshell failure aborts execution
  local subshell_rc=0
  (
    ( exit 77 ) || exit 77
    echo "unreachable"
  ) >/dev/null 2>&1 || subshell_rc=$?
  if [ "$subshell_rc" -ne 77 ]; then
    echo "self_test: subshell failure did not propagate (expected 77, got $subshell_rc)" >&2
    exit 1
  fi

  # 6. Copy / rsync failure stops publication
  cat <<'EOF' > "$fake_bin/rsync"
#!/usr/bin/env bash
exit 23
EOF
  chmod +x "$fake_bin/rsync"

  (
    export PATH="$fake_bin:$PATH"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    export TEST_REMOTE="$bare_repo"
    export TEST_TMP="$tmp"
    if "$0" root "$src" >/dev/null 2>&1; then
      echo "self_test: root publication unexpectedly succeeded with failing rsync" >&2
      exit 1
    fi
    if "$0" pr "$src" 42 >/dev/null 2>&1; then
      echo "self_test: pr publication unexpectedly succeeded with failing rsync" >&2
      exit 1
    fi
  ) || exit 1
  rm -f "$fake_bin/rsync"

  # 7. Git commit failure stops publication before push or build trigger
  local fake_git="$fake_bin/git"
  local real_git
  real_git="$(command -v git)"
  cat <<EOF > "$fake_git"
#!/usr/bin/env bash
for arg; do
  if [ "\$arg" = "commit" ]; then
    exit 1
  fi
done
exec "$real_git" "\$@"
EOF
  chmod +x "$fake_git"

  echo "modified content" >> "$src/index.html"
  (
    export PATH="$fake_bin:$PATH"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    export TEST_REMOTE="$bare_repo"
    export TEST_TMP="$tmp"
    if "$0" root "$src" >/dev/null 2>&1; then
      echo "self_test: root publication unexpectedly succeeded with failing git commit" >&2
      exit 1
    fi
  ) || exit 1
  rm -f "$fake_git"

  # 8. Git add failure stops publication before commit or push
  cat <<EOF > "$fake_git"
#!/usr/bin/env bash
for arg; do
  if [ "\$arg" = "add" ]; then
    exit 1
  fi
done
exec "$real_git" "\$@"
EOF
  chmod +x "$fake_git"

  echo "modified content for add test" >> "$src/index.html"
  (
    export PATH="$fake_bin:$PATH"
    export GITHUB_REPOSITORY="owner/repo"
    export GITHUB_TOKEN="dummy"
    export TEST_REMOTE="$bare_repo"
    export TEST_TMP="$tmp"
    if "$0" root "$src" >/dev/null 2>&1; then
      echo "self_test: root publication unexpectedly succeeded with failing git add" >&2
      exit 1
    fi
  ) || exit 1
  rm -f "$fake_git"

  # Exercise filesystem and orphan-checkout failures with isolated local remotes.
  python3 "$(dirname "$0")/../test/test_pages_publish.py" || return 1

  trap - EXIT
  rm -rf "$tmp"
  echo "publish-pages-branch self-test: PASS"
  return 0
}

if [ "${1:-}" = "--self-test" ]; then
  self_test
  exit 0
fi

mode="${1:?usage: publish-pages-branch.sh root <srcdir> | dev <srcdir> | pr <srcdir> <N> | rm <N>}"
: "${GITHUB_TOKEN:?GITHUB_TOKEN required}"
: "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY required}"

server="${GITHUB_SERVER_URL:-https://github.com}"
remote="${TEST_REMOTE:-https://x-access-token:${GITHUB_TOKEN}@${server#https://}/${GITHUB_REPOSITORY}.git}"

# Parse + validate args per mode up front (fail fast, before touching the remote).
src=""; num=""
case "$mode" in
  root) src="${2:?root needs <srcdir>}" ;;
  dev)  src="${2:?dev needs <srcdir>}" ;;
  pr)   src="${2:?pr needs <srcdir>}"; num="${3:?pr needs <N>}" ;;
  rm)   num="${2:?rm needs <N>}" ;;
  *)    echo "unknown mode '$mode'" >&2; exit 1 ;;
esac
if [ -n "$num" ] && ! [[ "$num" =~ ^[0-9]+$ ]]; then
  echo "PR number must be digits, got '$num'" >&2; exit 1
fi
[ -n "$src" ] && [ ! -d "$src" ] && { echo "source dir '$src' not found" >&2; exit 1; }

validate_dev_candidate() {
  [ "$mode" = dev ] || return 0
  local work="$1" main_row main_sha
  if ! [[ "${SOURCE_SHA:-}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "ERROR: dev publication requires an exact SOURCE_SHA" >&2
    return 1
  fi
  if ! python3 - "$work/dev/manifest.json" "$SOURCE_SHA" <<'PY'
import json, sys
try:
    with open(sys.argv[1]) as stream:
        manifest = json.load(stream)
    if not isinstance(manifest, dict) or manifest.get('sourceSha') != sys.argv[2]:
        raise ValueError('source mismatch')
except (OSError, ValueError):
    print('ERROR: dev manifest does not match SOURCE_SHA', file=sys.stderr)
    sys.exit(1)
PY
  then
    return 1
  fi
  # Query the same remote as the Pages push; a fresh clone alone only protects
  # the branch topology, not the age of the candidate being reapplied onto it.
  main_row="$(git ls-remote --exit-code "$remote" refs/heads/main 2>/dev/null)" || {
    echo "ERROR: cannot verify current main for dev publication" >&2
    return 1
  }
  main_sha="${main_row%%$'\t'*}"
  if [ "$main_row" != "$main_sha"$'\t'"refs/heads/main" ] ||
     [ "$main_sha" != "$SOURCE_SHA" ]; then
    echo "ERROR: refusing stale dev publication" >&2
    return 1
  fi
}

# Apply the requested change to a fresh gh-pages checkout at $1 (idempotent).
apply_changes() {
  local work="$1"
  case "$mode" in
    root)
      # Root files only — NEVER delete the PR/ preview tree (PR-owned) or the dev/ channel.
      rsync -a --delete --exclude='.git/' --exclude='PR/' --exclude='dev/' "$src"/ "$work"/ || return 1
      ;;
    dev)
      mkdir -p "$work/dev" || return 1
      rsync -a --delete --exclude='.git/' "$src"/ "$work/dev"/ || return 1
      ;;
    pr)
      local stage_pr="$work/PR/$num.tmp"
      rm -rf "$stage_pr" || return 1
      mkdir -p "$stage_pr" || return 1
      if ! rsync -a --delete --exclude='.git/' "$src"/ "$stage_pr"/; then
        rm -rf "$stage_pr" || return 1
        return 1
      fi
      rm -rf "${work:?}/PR/$num" || return 1
      # mv would nest the stage inside an unexpectedly retained destination.
      if [ -e "$work/PR/$num" ] || [ -L "$work/PR/$num" ]; then
        echo "ERROR: preview destination still exists after removal" >&2
        return 1
      fi
      mv "$stage_pr" "$work/PR/$num" || return 1
      ;;
    rm)
      rm -rf "${work:?}/PR/$num" || return 1
      ;;
  esac
}

commit_msg() {
  case "$mode" in
    root) echo "pages: publish site (root)" ;;
    dev)  echo "pages: publish site (dev channel)" ;;
    pr)   echo "pages: publish PR $num preview" ;;
    rm)   echo "pages: remove PR $num preview" ;;
  esac
}

for attempt in 1 2 3 4 5; do
  work="$(mktemp -d)"
  # Clone gh-pages if it exists, else start it as an orphan (first-ever publish).
  if git clone --quiet --depth 1 --branch gh-pages "$remote" "$work" 2>/dev/null; then :; else
    # An empty reachable remote can be cloned. Two failed clones do not prove
    # that Pages is absent; never manufacture an orphan from unreadable state.
    if ! git clone --quiet --depth 1 "$remote" "$work" 2>/dev/null; then
      echo "ERROR: cannot clone Pages or the default branch" >&2
      rm -rf "$work"
      exit 1
    fi
    if ! git -C "$work" checkout --orphan gh-pages 2>/dev/null; then
      echo "ERROR: cannot create the Pages orphan checkout" >&2
      rm -rf "$work"
      exit 1
    fi
    if ! tracked="$(git -C "$work" ls-files)"; then
      echo "ERROR: cannot inspect the fallback checkout" >&2
      rm -rf "$work"
      exit 1
    fi
    if [ -n "$tracked" ] && ! git -C "$work" rm -rfq . >/dev/null 2>&1; then
      echo "ERROR: cannot remove tracked default-branch files" >&2
      rm -rf "$work"
      exit 1
    fi
  fi
  git -C "$work" config user.name  "github-actions[bot]"
  git -C "$work" config user.email "41898282+github-actions[bot]@users.noreply.github.com"

  if ! apply_changes "$work"; then
    echo "ERROR: failed to apply changes" >&2
    rm -rf "$work"
    exit 1
  fi

  if ! git -C "$work" add -A; then
    echo "ERROR: git add failed" >&2
    rm -rf "$work"
    exit 1
  fi
  diff_rc=0
  git -C "$work" diff --cached --quiet || diff_rc=$?
  if [ "$diff_rc" -eq 0 ]; then
    if ! validate_dev_candidate "$work"; then
      rm -rf "$work"
      exit 1
    fi
    echo "gh-pages: nothing to change ($(commit_msg))"
    rm -rf "$work"
    trigger_pages_build || exit 1
    exit 0
  fi
  if [ "$diff_rc" -ne 1 ]; then
    echo "ERROR: cannot inspect staged Pages changes" >&2
    rm -rf "$work"
    exit 1
  fi
  if ! git -C "$work" commit --quiet -m "$(commit_msg)"; then
    echo "ERROR: git commit failed" >&2
    rm -rf "$work"
    exit 1
  fi

  if ! validate_dev_candidate "$work"; then
    rm -rf "$work"
    exit 1
  fi
  if git -C "$work" push --quiet "$remote" HEAD:gh-pages 2>/dev/null; then
    echo "gh-pages: $(commit_msg) — pushed (attempt $attempt)"
    rm -rf "$work"
    trigger_pages_build || exit 1
    exit 0
  fi
  echo "gh-pages: push rejected, retrying with a fresh clone (attempt $attempt)…" >&2
  rm -rf "$work"
  sleep $((attempt * 3))
done

echo "ERROR: could not push gh-pages after 5 attempts" >&2
exit 1
