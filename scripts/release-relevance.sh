#!/usr/bin/env bash
# Decide whether current main still contains release-relevant changes not represented by the
# authoritative root Pages snapshot, including the manifest actually served by GitHub Pages.
# Unlike github.event.before..sha, this cumulative baseline
# survives overlapping main runs: if firmware push A is made stale by later docs push B, B still
# sees A's firmware diff and completes the Release/Pages publication.
set -euo pipefail

contract_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
VERSION_RE='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'
SELF_TEST_PYTHON=""  # Set only by this script's isolated --self-test fixture.
RELEVANT_RE='^(cmake/admit-dependency-sources\.cmake$|scripts/dependency_source\.py$|scripts/nanopb-source\.json$|main/|patches/tesla-ble/|docs/(index\.html|installer-bootstrap\.mjs|serial-port-release\.mjs|web-installer\.mjs|vendor/|[^./][^/]*\.md$)|CMakeLists\.txt$|sdkconfig\.defaults(\.[a-z0-9]+)?$|dependencies\.lock\.[a-z0-9]+$|esp-idf-toolchain\.txt$|partitions\.csv$|version\.txt$|\.github/workflows/(build|signed-pr-preview|pr-preview-cleanup)\.yml$|scripts/firmware-(size|stack)-baseline\.json$|scripts/ota-signing-public-key\.sha256$|scripts/tesla-ble-patch-state\.json$|scripts/(ci-build-all|ci-build-verify|ci-sign-artifacts|next-version|select-release-version|release-relevance|test-release-contract|check-reproducible-build|idf-docker|idf-version|build-pages|publish-pages-branch|apply-tesla-ble-patches)\.sh$|scripts/(check-idf-container-mounts|apply-tesla-ble-patches|check-build-artifact-inventory|check-build-gate-contract|check-pages-manifest|check-pages-source|wait-pages-build|check-dev-pages|check-release-pages-bytes|check-release-assets|check-signed-root-inventory|prepare-reused-release|check-published-release|check-firmware-artifacts|check-partition-contract|check-dependency-contract|check-otadata-contract|check-build-semantics|check-stack-usage|check-sdkconfig-defaults|report-firmware-size|generate-ota-changelog)\.py$)'

validate_sha() {
  local repo_root="$1" sha="$2"
  [[ "$sha" =~ ^[0-9a-f]{40}$ ]] \
    && git -C "$repo_root" cat-file -e "${sha}^{commit}" 2>/dev/null
}

relevant_paths() {
  local path relevant=no
  while IFS= read -r -d '' path; do
    if [[ "$path" =~ $RELEVANT_RE ]]; then relevant=yes; fi
  done
  printf '%s\n' "$relevant"
}

parse_manifest_identity() {
  local py="${1:-python3}"
  "$py" -c '
import json, sys
try:
    d = json.load(sys.stdin)
    if not (isinstance(d, dict) and d.get("name") == "tesla-key-esp32" and d.get("layoutVersion") == 2):
        sys.exit(1)
    b = d.get("builds")
    if not (isinstance(b, list) and len(b) == 4):
        sys.exit(1)
    s = d.get("sourceSha")
    v = d.get("version")
    if not (isinstance(s, str) and isinstance(v, str)):
        sys.exit(1)
    sys.stdout.write(f"{s}\t{v}\n")
    sys.exit(0)
except Exception:
    sys.exit(1)
' 2>/dev/null
}

validate_release_json() {
  local py="$1" tag="$2" sha="$3" v="$4"
  "$py" -c '
import json, re, sys
tag, sha, v = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    r = json.load(sys.stdin)
    if not (isinstance(r, dict)
            and r.get("tag_name") == tag
            and r.get("target_commitish") == sha
            and r.get("draft") is False
            and r.get("prerelease") is False
            and r.get("immutable") is True):
        sys.exit(1)
    assets = r.get("assets")
    if not isinstance(assets, list):
        sys.exit(1)
    expected = [
        f"tesla-key-esp32-{v}-merged.bin",
        f"tesla-key-esp32-s3-{v}-merged.bin",
        f"tesla-key-esp32-c3-{v}-merged.bin",
        f"tesla-key-esp32-c6-{v}-merged.bin",
    ]
    digest_re = re.compile(r"^sha256:[0-9a-f]{64}$")
    for name in expected:
        matches = [a for a in assets if isinstance(a, dict) and a.get("name") == name]
        if len(matches) != 1:
            sys.exit(1)
        a = matches[0]
        if not (isinstance(a.get("id"), (int, float)) and not isinstance(a.get("id"), bool)):
            sys.exit(1)
        if not (isinstance(a.get("size"), (int, float)) and not isinstance(a.get("size"), bool) and a["size"] > 0):
            sys.exit(1)
        digest = a.get("digest")
        if not (isinstance(digest, str) and digest_re.match(digest)):
            sys.exit(1)
    sys.exit(0)
except Exception:
    sys.exit(1)
' "$tag" "$sha" "$v" 2>/dev/null
}

parse_pages_url() {
  local py="${1:-python3}"
  "$py" -c '
import json, re, sys
try:
    d = json.load(sys.stdin)
    url = d.get("html_url") if isinstance(d, dict) else None
    if isinstance(url, str) and re.match(r"^https://[^\s?#]+/?$", url):
        sys.stdout.write(f"{url}\n")
        sys.exit(0)
except Exception:
    pass
sys.exit(1)
' 2>/dev/null
}

# find_published_pages_baseline <repo> <current-sha>
# Print a source SHA only when ALL authorities agree:
#   * root gh-pages manifest identity (layout/version/sourceSha),
#   * the same identity from the live URL returned by the repository Pages API,
#   * the matching immutable Git tag,
#   * the latest immutable, non-draft/non-prerelease GitHub Release with all four digest-bound
#     merged assets,
#   * and ancestry of that source SHA in the current main snapshot.
# Any missing, stale or unreadable authority is not a usable baseline.
find_published_pages_baseline() {
  local repo_root="$1" current_sha="$2" repository identity source_sha version tag tag_sha
  local release_json pages_json live_base live_url live_manifest live_identity live_source live_version
  local acceptance_dir acceptance_json release_metadata_file python_cmd published_commit branch_manifest_file
  python_cmd="${SELF_TEST_PYTHON:-python3}"
  validate_sha "$repo_root" "$current_sha" || return 2
  command -v "$python_cmd" >/dev/null 2>&1 && command -v gh >/dev/null 2>&1 \
    && command -v curl >/dev/null 2>&1 || return 2
  repository="${GITHUB_REPOSITORY:-}"
  [[ "$repository" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] || return 2

  git -C "$repo_root" fetch --quiet --tags --force --prune --prune-tags origin || return 2
  git -C "$repo_root" fetch --quiet --no-tags --force origin \
    refs/heads/gh-pages:refs/remotes/origin/gh-pages || return 2
  published_commit="$(git -C "$repo_root" rev-parse --verify 'refs/remotes/origin/gh-pages^{commit}' 2>/dev/null)" || return 2
  [[ "$published_commit" =~ ^[0-9a-f]{40}$ ]] || return 2
  acceptance_dir="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tesla-release-baseline.XXXXXX")" \
    || return 2
  branch_manifest_file="$acceptance_dir/branch_manifest.json"
  if ! git -C "$repo_root" show "$published_commit:manifest.json" > "$branch_manifest_file" 2>/dev/null \
      || ! git -C "$repo_root" show "$published_commit:changelog.json" > "$acceptance_dir/changelog.json" 2>/dev/null; then
    rm -rf -- "$acceptance_dir"
    return 2
  fi
  identity="$(parse_manifest_identity "$python_cmd" < "$branch_manifest_file")" || { rm -rf -- "$acceptance_dir"; return 2; }
  source_sha="${identity%%$'\t'*}"
  version="${identity#*$'\t'}"
  validate_sha "$repo_root" "$source_sha" || { rm -rf -- "$acceptance_dir"; return 2; }
  [[ "$version" =~ $VERSION_RE ]] || { rm -rf -- "$acceptance_dir"; return 2; }
  git -C "$repo_root" merge-base --is-ancestor "$source_sha" "$current_sha" || { rm -rf -- "$acceptance_dir"; return 2; }
  "$python_cmd" "$contract_root/scripts/generate-ota-changelog.py" \
    --validate "$acceptance_dir/changelog.json" --version "$version" >/dev/null \
    || { rm -rf -- "$acceptance_dir"; return 2; }

  tag="v$version"
  tag_sha="$(git -C "$repo_root" rev-parse "${tag}^{commit}" 2>/dev/null)" || { rm -rf -- "$acceptance_dir"; return 2; }
  [[ "$tag_sha" == "$source_sha" ]] || { rm -rf -- "$acceptance_dir"; return 2; }

  release_json="$(gh api "repos/$repository/releases/latest" 2>/dev/null)" || { rm -rf -- "$acceptance_dir"; return 2; }
  printf '%s' "$release_json" \
    | validate_release_json "$python_cmd" "$tag" "$source_sha" "$version" || { rm -rf -- "$acceptance_dir"; return 2; }
  release_metadata_file="$(mktemp "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tesla-release-metadata.XXXXXX")" \
    || { rm -rf -- "$acceptance_dir"; return 2; }
  if ! printf '%s' "$release_json" > "$release_metadata_file" \
      || ! "$python_cmd" "$contract_root/scripts/check-release-assets.py" \
        "$release_metadata_file" --metadata-only \
        --version "$version" --source-sha "$source_sha" \
        --expect-state published-immutable >/dev/null; then
    rm -f -- "$release_metadata_file"
    rm -rf -- "$acceptance_dir"
    return 2
  fi
  rm -f -- "$release_metadata_file"

  # Branch+Release agreement alone does not prove that the OTA channel users actually fetch is
  # current: the configured Pages source can drift, a branch build can fail, or CDN publication can
  # lag. Read the configured Pages URL and require the live root manifest to advertise the exact
  # same stable release identity. Any API, TLS, HTTP, JSON or cache-liveness uncertainty fails
  # closed so a same-SHA rerun retries publication/deployment.
  pages_json="$(gh api "repos/$repository/pages" 2>/dev/null)" || { rm -rf -- "$acceptance_dir"; return 2; }
  printf '%s' "$pages_json" \
    | "$python_cmd" "$contract_root/scripts/check-pages-source.py" - >/dev/null || { rm -rf -- "$acceptance_dir"; return 2; }
  live_base="$(printf '%s' "$pages_json" | parse_pages_url "$python_cmd")" || { rm -rf -- "$acceptance_dir"; return 2; }
  live_url="${live_base%/}/manifest.json?release-relevance=$current_sha"
  live_manifest="$(curl --fail --location --silent --show-error --max-time 20 \
    --retry 2 --retry-all-errors -H 'Cache-Control: no-cache' "$live_url" 2>/dev/null)" || { rm -rf -- "$acceptance_dir"; return 2; }
  live_identity="$(printf '%s' "$live_manifest" | parse_manifest_identity "$python_cmd")" || { rm -rf -- "$acceptance_dir"; return 2; }
  live_source="${live_identity%%$'\t'*}"
  live_version="${live_identity#*$'\t'}"
  [[ "$live_source" == "$source_sha" && "$live_version" == "$version" ]] || { rm -rf -- "$acceptance_dir"; return 2; }

  # A matching manifest is only an intent signal. It must never suppress reconciliation until all
  # 16 live Pages parts are byte-identical to the four immutable Release images. Reuse the same
  # bounded validator as the post-publication acceptance step so a partial CDN update (new
  # manifest, stale binary) keeps the cumulative release gate red on the next main run.
  acceptance_json="$acceptance_dir/release.json"
  if ! printf '%s' "$release_json" > "$acceptance_json" \
      || ! "$python_cmd" "$contract_root/scripts/check-published-release.py" \
        --pages-base-url "$live_base" \
        --release-base-url "https://github.com/$repository/releases/download/$tag" \
        --release-json "$acceptance_json" \
        --expected-changelog-file "$acceptance_dir/changelog.json" \
        --version "$version" --source-sha "$source_sha" \
        --attempts 1 --interval 0 --timeout 20 >/dev/null; then
    rm -rf -- "$acceptance_dir"
    return 2
  fi
  rm -rf -- "$acceptance_dir"

  printf '%s\n' "$source_sha"
}

changed_since_release() {
  local repo_root="$1" current_sha="$2" baseline files
  validate_sha "$repo_root" "$current_sha" || {
    echo "invalid current source SHA: $current_sha" >&2
    return 2
  }
  if ! baseline="$(find_published_pages_baseline "$repo_root" "$current_sha")"; then
    echo "no authoritative Release/Pages baseline; release relevance fails closed to yes" >&2
    printf 'yes\n'
    return 0
  fi
  # Disable rename folding so both the deletion and addition paths are considered. Otherwise a
  # `main/foo.cpp -> docs/foo.cpp` move is rendered only as docs/foo.cpp and loses the firmware
  # side of the change, suppressing a required Release.
  files="$(git -C "$repo_root" diff --name-only -z --no-renames "$baseline" "$current_sha" | relevant_paths)" || {
    echo "cannot diff Release/Pages baseline $baseline to $current_sha" >&2
    printf 'yes\n'
    return 0
  }
  printf '%s\n' "$files"
}

# find_published_dev_baseline <repo> <current-sha>
# Print a source SHA only when ALL dev authorities agree:
#   * gh-pages dev/manifest.json identity (layout/version/sourceSha),
#   * the same identity from the live URL returned by the repository Pages API (/dev/manifest.json),
#   * ancestry of that source SHA in the current main snapshot,
#   * and every one of the 16 served firmware parts byte-identical to the size/SHA-256 the live
#     manifest declares (scripts/check-dev-pages.py --verify-live).
# A missing, stale or unreadable authority returns 2 so the caller can fall back to the Release
# baseline. Return 3 is different: the identities AGREE but the served bytes do not match their own
# manifest (an old or damaged binary behind a current manifest). That channel is a known-broken
# state, not merely an unknown one, so the caller must keep the reconciliation open (fail closed).
find_published_dev_baseline() {
  local repo_root="$1" current_sha="$2" repository manifest identity source_sha version
  local pages_json live_base live_url live_manifest live_identity live_source live_version
  local python_cmd dev_version_re branch_manifest_sha live_manifest_sha published_commit
  python_cmd="${SELF_TEST_PYTHON:-python3}"
  dev_version_re='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?$'
  validate_sha "$repo_root" "$current_sha" || return 2
  command -v "$python_cmd" >/dev/null 2>&1 && command -v gh >/dev/null 2>&1 \
    && command -v curl >/dev/null 2>&1 || return 2
  repository="${GITHUB_REPOSITORY:-}"
  [[ "$repository" =~ ^[0-9A-Za-z_.-]+/[0-9A-Za-z_.-]+$ ]] || return 2

  git -C "$repo_root" fetch --quiet --tags --force --prune --prune-tags origin || return 2
  git -C "$repo_root" fetch --quiet --no-tags --force origin \
    refs/heads/gh-pages:refs/remotes/origin/gh-pages || return 2
  local tmp_dir
  tmp_dir="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tesla-dev-baseline.XXXXXX")" || return 2
  local branch_manifest_file="$tmp_dir/branch_manifest.json"
  local live_manifest_file="$tmp_dir/live_manifest.json"
  local branch_changelog_file="$tmp_dir/changelog.json"

  published_commit="$(git -C "$repo_root" rev-parse --verify 'refs/remotes/origin/gh-pages^{commit}' 2>/dev/null)" || { rm -rf "$tmp_dir"; return 2; }
  [[ "$published_commit" =~ ^[0-9a-f]{40}$ ]] || { rm -rf "$tmp_dir"; return 2; }
  if ! git -C "$repo_root" show "$published_commit:dev/manifest.json" > "$branch_manifest_file" 2>/dev/null; then
    rm -rf "$tmp_dir"
    return 2
  fi

  identity="$(parse_manifest_identity "$python_cmd" < "$branch_manifest_file")" || { rm -rf "$tmp_dir"; return 2; }
  source_sha="${identity%%$'\t'*}"
  version="${identity#*$'\t'}"
  validate_sha "$repo_root" "$source_sha" || { rm -rf "$tmp_dir"; return 2; }
  [[ "$version" =~ $dev_version_re ]] || { rm -rf "$tmp_dir"; return 2; }
  git -C "$repo_root" merge-base --is-ancestor "$source_sha" "$current_sha" || { rm -rf "$tmp_dir"; return 2; }
  if ! git -C "$repo_root" show "$published_commit:dev/changelog.json" > "$branch_changelog_file" 2>/dev/null \
      || ! "$python_cmd" "$contract_root/scripts/generate-ota-changelog.py" \
        --validate "$branch_changelog_file" --version "$version" >/dev/null; then
    rm -rf "$tmp_dir"
    return 3
  fi

  pages_json="$(gh api "repos/$repository/pages" 2>/dev/null)" || { rm -rf "$tmp_dir"; return 2; }
  printf '%s' "$pages_json" \
    | "$python_cmd" "$contract_root/scripts/check-pages-source.py" - >/dev/null || { rm -rf "$tmp_dir"; return 2; }
  live_base="$(printf '%s' "$pages_json" | parse_pages_url "$python_cmd")" || { rm -rf "$tmp_dir"; return 2; }
  live_url="${live_base%/}/dev/manifest.json?release-relevance=$current_sha"

  if ! curl --fail --location --silent --show-error --max-time 20 \
    --retry 2 --retry-all-errors -H 'Cache-Control: no-cache' "$live_url" -o "$live_manifest_file" 2>/dev/null; then
    rm -rf "$tmp_dir"
    return 2
  fi

  live_identity="$(parse_manifest_identity "$python_cmd" < "$live_manifest_file")" || { rm -rf "$tmp_dir"; return 2; }
  live_source="${live_identity%%$'\t'*}"
  live_version="${live_identity#*$'\t'}"
  if [[ "$live_source" != "$source_sha" || "$live_version" != "$version" ]]; then
    rm -rf "$tmp_dir"
    return 2
  fi

  branch_manifest_sha="$("$python_cmd" -c 'import hashlib, sys; sys.stdout.write(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$branch_manifest_file")"
  live_manifest_sha="$("$python_cmd" -c 'import hashlib, sys; sys.stdout.write(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$live_manifest_file")"
  if [[ "$branch_manifest_sha" != "$live_manifest_sha" ]]; then
    rm -rf "$tmp_dir"
    return 3
  fi

  # Identity alone proves only that the manifest was published, not that the binaries behind it
  # are. Compare every served part with what the manifest declares.
  if ! "$python_cmd" "$contract_root/scripts/check-dev-pages.py" --verify-live \
    --pages-base-url "${live_base%/}" --version "$version" --source-sha "$source_sha" \
    --expected-manifest-sha "$branch_manifest_sha" \
    --expected-changelog-file "$branch_changelog_file" \
    --attempts 1 --interval 0 --timeout 20 >/dev/null; then
    rm -rf "$tmp_dir"
    return 3
  fi

  rm -rf "$tmp_dir"
  printf '%s\n' "$source_sha"
}

changed_since_dev() {
  local repo_root="$1" current_sha="$2" baseline files
  validate_sha "$repo_root" "$current_sha" || {
    echo "invalid current source SHA: $current_sha" >&2
    return 2
  }
  local baseline_rc=0
  baseline="$(find_published_dev_baseline "$repo_root" "$current_sha")" || baseline_rc=$?
  if [[ "$baseline_rc" -eq 3 ]]; then
    echo "dev channel serves bytes that do not match its manifest; reconciliation stays open" >&2
    printf 'yes\n'
    return 0
  fi
  if [[ "$baseline_rc" -ne 0 ]]; then
    echo "no authoritative dev/Pages baseline; falling back to Release/Pages baseline" >&2
    changed_since_release "$repo_root" "$current_sha"
    return $?
  fi
  # Disable rename folding so both the deletion and addition paths are considered.
  files="$(git -C "$repo_root" diff --name-only -z --no-renames "$baseline" "$current_sha" | relevant_paths)" || {
    echo "cannot diff dev/Pages baseline $baseline to $current_sha" >&2
    printf 'yes\n'
    return 0
  }
  printf '%s\n' "$files"
}

changed_for_pr() {
  local repo_root="$1" base_sha="$2" head_sha="$3" files
  validate_sha "$repo_root" "$base_sha" || {
    echo "invalid PR base SHA: $base_sha; failing closed to yes" >&2
    printf 'yes\n'
    return 0
  }
  validate_sha "$repo_root" "$head_sha" || {
    echo "invalid PR head SHA: $head_sha; failing closed to yes" >&2
    printf 'yes\n'
    return 0
  }
  files="$(git -C "$repo_root" diff --name-only -z --no-renames "$base_sha" "$head_sha" | relevant_paths)" || {
    echo "cannot diff PR base $base_sha to $head_sha; failing closed to yes" >&2
    printf 'yes\n'
    return 0
  }
  printf '%s\n' "$files"
}

write_fake_release() {
  local path="$1" version="$2" sha="$3"
  python3 - "$contract_root/scripts/check-release-assets.py" "$path" "$version" "$sha" <<'PY'
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

spec = importlib.util.spec_from_file_location("release_assets", sys.argv[1])
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix="release-relevance-assets-") as directory:
    root = Path(directory)
    for asset in module.expected_local_assets(root, sys.argv[3]).values():
        asset.parent.mkdir(parents=True, exist_ok=True)
        asset.write_bytes(b"x")
    release = module.make_full_release(
        root, sys.argv[3], sys.argv[4], "published-immutable"
    )
    release["id"] = 7
    Path(sys.argv[2]).write_text(json.dumps(release), encoding="utf-8")
PY
}

write_pages_manifest() {
  local path="$1" version="$2" sha="$3"
  printf '{"name":"tesla-key-esp32","layoutVersion":2,"sourceSha":"%s","version":"%s","builds":[{},{},{},{}]}\n' \
    "$sha" "$version" > "$path"
  if [[ "${path##*/}" == manifest.json ]]; then
    printf '{"version":"%s","changelog":"One bounded change."}\n' "$version" > "${path%/*}/changelog.json"
  fi
}

self_test() {
  local tmp remote fakebin fake_release fake_live sha_r sha_a sha_b sha_stack
  local sha_dependency sha_otadata sha_signed_root sha_public_key sha_reuse sha_c got real_python
  unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_PREFIX
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/tesla-release-relevance.XXXXXX")"
  remote="$tmp.remote.git"
  trap 'rm -rf -- "$tmp" "$remote"' RETURN
  real_python="$(command -v python3)" || return 1

  git -C "$tmp" init -q
  git -C "$tmp" config user.name test
  git -C "$tmp" config user.email test@example.invalid
  git -C "$tmp" config commit.gpgsign false
  git -C "$tmp" config tag.gpgsign false
  mkdir -p "$tmp/main" "$tmp/docs"
  printf 'released\n' > "$tmp/main/firmware.cpp"
  git -C "$tmp" add main/firmware.cpp
  git -C "$tmp" commit -qm released
  git -C "$tmp" branch -M main
  sha_r="$(git -C "$tmp" rev-parse HEAD)"
  git -C "$tmp" tag v1.0.0
  git init --bare -q "$remote"
  git -C "$tmp" remote add origin "$remote"
  git -C "$tmp" push -q origin main --tags

  git -C "$tmp" checkout -qb gh-pages
  write_pages_manifest "$tmp/manifest.json" 1.0.0 "$sha_r"
  git -C "$tmp" add manifest.json changelog.json
  git -C "$tmp" commit -qm pages-r
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main

  fakebin="$tmp/fakebin"
  fake_release="$tmp/fake-release.json"
  fake_live="$tmp/fake-live-manifest.json"
  mkdir -p "$fakebin"
  printf '%s\n' '#!/usr/bin/env bash' 'set -eu' \
    '[ "$1" = api ]' \
    'case "$2" in' \
    '  */releases/latest) cat "$GH_FAKE_RELEASE" ;;' \
    '  */pages) [ "${GH_FAKE_PAGES_FAIL:-0}" != 1 ] || exit 1; if [ "${GH_FAKE_PAGES_WORKFLOW:-0}" = 1 ]; then mode=workflow; else mode=legacy; fi; printf '\''{"build_type":"%s","source":{"branch":"gh-pages","path":"/"},"html_url":"https://owner.invalid/repo/"}\n'\'' "$mode" ;;' \
    '  *) exit 1 ;;' \
    'esac' > "$fakebin/gh"
  chmod +x "$fakebin/gh"
  cat > "$fakebin/curl" <<'CURL_WRAPPER'
#!/usr/bin/env bash
set -eu
out=""
src=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2 ;;
    *)
      if [[ "$1" == */dev/manifest.json* ]] && [ -n "${GH_FAKE_LIVE_DEV:-}" ]; then
        src="$GH_FAKE_LIVE_DEV"
      fi
      shift
      ;;
  esac
done
[ -n "$src" ] || src="$GH_FAKE_LIVE"
if [ -n "$out" ]; then
  cat "$src" > "$out"
else
  cat "$src"
fi
CURL_WRAPPER
  chmod +x "$fakebin/curl"
  cat > "$fakebin/python3" <<'PYTHON_WRAPPER'
#!/usr/bin/env bash
set -eu
require_changelog_input() {
  local script="$1" expected="" version="" i next
  for ((i=1; i<=$#; i++)); do
    case "${!i}" in
      --expected-changelog-file) next=$((i+1)); expected="${!next}" ;;
      --version) next=$((i+1)); version="${!next}" ;;
    esac
  done
  [ -n "$expected" ] && [ -n "$version" ] || exit 98
  "$GH_FAKE_REAL_PYTHON" "${script%/*}/generate-ota-changelog.py" \
    --validate "$expected" --version "$version" >/dev/null
  [ "${GH_FAKE_CHANGELOG_MISMATCH:-0}" != 1 ] || exit 1
}
case "${1:-}" in
  */check-dev-pages.py)
    require_changelog_input "$@"
    # Same wiring fixture for the dev channel: the validator owns the byte-level mutations.
    if [[ "${GH_FAKE_DEV_BYTES_STALE:-0}" == 1 ]]; then
      exit 1
    fi
    for ((i=1; i<=$#; i++)); do
      if [[ "${!i}" == "--expected-manifest-sha" ]]; then
        next=$((i+1))
        exp_sha="${!next}"
        actual_sha="$("$GH_FAKE_REAL_PYTHON" -c 'import hashlib, sys; sys.stdout.write(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "${GH_FAKE_LIVE_DEV:-$GH_FAKE_LIVE}")"
        if [[ "$exp_sha" != "$actual_sha" ]]; then
          echo "fake check-dev-pages: expected-manifest-sha mismatch: $exp_sha vs $actual_sha" >&2
          exit 1
        fi
      fi
    done
    exit 0
    ;;
  */check-published-release.py)
    require_changelog_input "$@"
    if [ -n "${GH_FAKE_EXPECTED_ROOT_MANIFEST:-}" ]; then
      expected=""
      for ((i=1; i<=$#; i++)); do
        if [[ "${!i}" == --expected-changelog-file ]]; then
          next=$((i+1)); expected="${!next}"
        fi
      done
      cmp "${expected%/*}/branch_manifest.json" "$GH_FAKE_EXPECTED_ROOT_MANIFEST" || exit 97
    fi
    # The validator owns byte-level mutation tests. This wiring fixture makes a partial live
    # snapshot observable without opening a real HTTPS listener in the shell self-test.
    if [[ "${GH_FAKE_LIVE_BYTES_STALE:-0}" == 1 ]]; then
      exit 1
    fi
    exit 0
    ;;
esac
exec "$GH_FAKE_REAL_PYTHON" "$@"
PYTHON_WRAPPER
  chmod +x "$fakebin/python3"
  SELF_TEST_PYTHON="$fakebin/python3"
  export GH_FAKE_REAL_PYTHON="$real_python"
  hash -r
  write_fake_release "$fake_release" 1.0.0 "$sha_r"
  write_pages_manifest "$fake_live" 1.0.0 "$sha_r"

  # Timeline: A changes firmware, then unshipped Markdown B advances main before A can publish.
  # The event before..B diff omits firmware, but the authoritative baseline R..B includes A.
  printf 'firmware-a\n' >> "$tmp/main/firmware.cpp"
  git -C "$tmp" add main/firmware.cpp
  git -C "$tmp" commit -qm firmware-a
  sha_a="$(git -C "$tmp" rev-parse HEAD)"
  printf 'unshipped-b\n' > "$tmp/README.md"
  git -C "$tmp" add README.md
  git -C "$tmp" commit -qm docs-b
  sha_b="$(git -C "$tmp" rev-parse HEAD)"
  git -C "$tmp" push -q origin main
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "firmware A + docs B timeline lost cumulative release relevance: $got" >&2; return 1;
  }

  # Partial publication: Release A/tag already exist, but root Pages still advertises R. The
  # mismatch is not an authoritative baseline and must schedule reconciliation from current main.
  git -C "$tmp" tag v1.0.1 "$sha_a"
  git -C "$tmp" push -q origin v1.0.1
  write_fake_release "$fake_release" 1.0.1 "$sha_a"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "Release-created/Pages-old reconciliation was skipped: $got" >&2; return 1;
  }

  # A successful branch/Release publication is not enough until branch-backed Pages serves it. If
  # the branch build or CDN update lags/fails, a same-SHA rerun must remain relevant and retry rather
  # than going green while the public OTA manifest still advertises R.
  git -C "$tmp" checkout -q gh-pages
  write_pages_manifest "$tmp/manifest.json" 1.0.1 "$sha_a"
  git -C "$tmp" add manifest.json changelog.json
  git -C "$tmp" commit -qm pages-a
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "Release/gh-pages A with live Pages still R skipped deployment reconciliation: $got" >&2
    return 1
  }

  # Once branch, live Pages and latest Release all authoritatively represent A, unshipped
  # Markdown B is not relevant and must not cut another Release.
  write_pages_manifest "$fake_live" 1.0.1 "$sha_a"
  got="$(
    export PATH="$fakebin:$PATH"
    export GITHUB_REPOSITORY=owner/repo
    export GH_FAKE_RELEASE="$fake_release"
    export GH_FAKE_LIVE="$fake_live"
    export GH_FAKE_LIVE_BYTES_STALE=1
    changed_since_release "$tmp" "$sha_b"
  )"
  [[ "$got" == yes ]] || {
    echo "new live manifest with a stale firmware part suppressed reconciliation: $got" >&2
    return 1
  }
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == no ]] || {
    echo "fully published A incorrectly made docs-only B release-relevant: $got" >&2; return 1;
  }
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_CHANGELOG_MISMATCH=1 \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "root metadata mismatch suppressed reconciliation: $got" >&2; return 1;
  }
  git -C "$tmp" checkout -q gh-pages
  cp "$tmp/changelog.json" "$fakebin/held-root-changelog.json"
  git -C "$tmp" rm -q changelog.json
  git -C "$tmp" commit -qm root-metadata-missing
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "missing root metadata suppressed reconciliation: $got" >&2; return 1;
  }
  git -C "$tmp" checkout -q gh-pages
  cp "$fakebin/held-root-changelog.json" "$tmp/changelog.json"
  git -C "$tmp" add changelog.json
  git -C "$tmp" commit -qm root-metadata-restored
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == no ]] || {
    echo "restored root metadata failed acceptance: $got" >&2; return 1;
  }

  "$real_python" -c '
import json, sys
p = sys.argv[1]
with open(p, "r+", encoding="utf-8") as f:
    d = json.load(f)
    d["assets"] = d["assets"][:-1]
    f.seek(0); f.truncate(); json.dump(d, f)
' "$fake_release"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "incomplete immutable Release became a publication baseline: $got" >&2; return 1;
  }
  write_fake_release "$fake_release" 1.0.1 "$sha_a"
  "$real_python" -c '
import json, sys
p = sys.argv[1]
with open(p, "r+", encoding="utf-8") as f:
    d = json.load(f)
    d["assets"].append({"id": 999, "name": "extra.bin", "size": 1, "digest": "sha256:" + "0" * 64})
    f.seek(0); f.truncate(); json.dump(d, f)
' "$fake_release"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "extra immutable Release asset became a publication baseline: $got" >&2; return 1;
  }
  write_fake_release "$fake_release" 1.0.1 "$sha_a"

  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_PAGES_WORKFLOW=1 \
    changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "Actions Pages source mode failed open to a branch-backed baseline: $got" >&2; return 1;
  }

  # A published/stable Release is not an authority until the API explicitly reports immutable
  # true. Both false and an absent field must fail closed so older API fixtures or a repository
  # without immutable releases cannot suppress required publication reconciliation.
  "$real_python" -c '
import json, sys
p = sys.argv[1]
with open(p, "r+", encoding="utf-8") as f:
    d = json.load(f)
    d["immutable"] = False
    f.seek(0); f.truncate(); json.dump(d, f)
' "$fake_release"
  if validate_release_json "$real_python" v1.0.1 "$sha_a" 1.0.1 < "$fake_release"; then
    echo "direct immutable false authority was accepted" >&2; return 1;
  fi
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "mutable Release authority failed open to no: $got" >&2; return 1;
  }
  write_fake_release "$fake_release" 1.0.1 "$sha_a"
  "$real_python" -c '
import json, sys
p = sys.argv[1]
with open(p, "r+", encoding="utf-8") as f:
    d = json.load(f)
    d.pop("immutable", None)
    f.seek(0); f.truncate(); json.dump(d, f)
' "$fake_release"
  if validate_release_json "$real_python" v1.0.1 "$sha_a" 1.0.1 < "$fake_release"; then
    echo "direct immutable missing authority was accepted" >&2; return 1;
  fi
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "Release authority without immutable field failed open to no: $got" >&2; return 1;
  }
  write_fake_release "$fake_release" 1.0.1 "$sha_a"

  # Reviewed build-resource baselines affect what bytes are allowed to ship. Pin the real stack
  # filename: a name drift here would let its change bypass the cumulative release decision.
  mkdir -p "$tmp/scripts"
  local contract_path
  for contract_path in scripts/check-idf-container-mounts.py scripts/apply-tesla-ble-patches.py scripts/tesla-ble-patch-state.json scripts/wait-pages-build.py; do
    printf '%s\n' "$contract_path" | grep -Eq "$RELEVANT_RE" || {
      echo "build admission input is not release-relevant: $contract_path" >&2; return 1;
    }
  done
  printf '{"schemaVersion":1}\n' > "$tmp/scripts/firmware-stack-baseline.json"
  git -C "$tmp" add scripts/firmware-stack-baseline.json
  git -C "$tmp" commit -qm firmware-stack-baseline
  sha_stack="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_stack")"
  [[ "$got" == yes ]] || {
    echo "firmware-stack baseline change was not release-relevant: $got" >&2; return 1;
  }

  # The trusted signer consumes both validators. A hardening or accidental weakening of either
  # contract must reconcile the published channel even when no firmware source changed.
  printf '# dependency contract\n' > "$tmp/scripts/check-dependency-contract.py"
  git -C "$tmp" add scripts/check-dependency-contract.py
  git -C "$tmp" commit -qm dependency-contract
  sha_dependency="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_dependency")"
  [[ "$got" == yes ]] || {
    echo "dependency contract change was not release-relevant: $got" >&2; return 1;
  }

  printf '# otadata contract\n' > "$tmp/scripts/check-otadata-contract.py"
  git -C "$tmp" add scripts/check-otadata-contract.py
  git -C "$tmp" commit -qm otadata-contract
  sha_otadata="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_otadata")"
  [[ "$got" == yes ]] || {
    echo "otadata contract change was not release-relevant: $got" >&2; return 1;
  }

  printf '# signed root inventory contract\n' > "$tmp/scripts/check-signed-root-inventory.py"
  git -C "$tmp" add scripts/check-signed-root-inventory.py
  git -C "$tmp" commit -qm signed-root-inventory-contract
  sha_signed_root="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_signed_root")"
  [[ "$got" == yes ]] || {
    echo "signed root inventory contract change was not release-relevant: $got" >&2; return 1;
  }

  # The reviewed digest is the production OTA authority. A pin change is firmware/recovery
  # relevant even if no source or workflow changed; otherwise the public channel could continue
  # advertising bytes under an authority the repository no longer accepts.
  printf '%064d\n' 0 > "$tmp/scripts/ota-signing-public-key.sha256"
  git -C "$tmp" add scripts/ota-signing-public-key.sha256
  git -C "$tmp" commit -qm ota-signing-public-key-pin
  sha_public_key="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_public_key")"
  [[ "$got" == yes ]] || {
    echo "OTA signing public-key pin change was not release-relevant: $got" >&2; return 1;
  }

  printf '# immutable release reuse contract\n' > "$tmp/scripts/prepare-reused-release.py"
  git -C "$tmp" add scripts/prepare-reused-release.py
  git -C "$tmp" commit -qm reused-release-contract
  sha_reuse="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_reuse")"
  [[ "$got" == yes ]] || {
    echo "immutable Release reuse contract change was not release-relevant: $got" >&2; return 1;
  }

  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_PAGES_FAIL=1 changed_since_release "$tmp" "$sha_b")"
  [[ "$got" == yes ]] || {
    echo "unreadable Pages configuration failed open to no: $got" >&2; return 1;
  }

  # Rename detection must not hide the deletion side of a firmware->docs move. Git's default
  # --name-only rename rendering reports only the destination and would otherwise suppress the
  # release even though main/ changed materially.
  git -C "$tmp" mv main/firmware.cpp docs/firmware.cpp
  git -C "$tmp" commit -qm move-firmware-out-of-main
  sha_c="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_c")"
  [[ "$got" == yes ]] || {
    echo "firmware-to-docs rename lost the relevant source deletion: $got" >&2; return 1;
  }

  # Unreadable/mismatched authority never suppresses a needed publication.
  if PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$tmp/missing.json" \
      GH_FAKE_LIVE="$fake_live" \
      changed_since_release "$tmp" "$sha_b" | grep -Fxq no; then
    echo "unreadable Release authority failed open to no" >&2
    return 1
  fi

  # PR relevance: shipped Markdown and firmware produce yes; a bad SHA fails closed.
  git -C "$tmp" checkout -q -b pr-branch
  echo "test doc edit" >> "$tmp/docs/README.md"
  git -C "$tmp" add docs/README.md
  git -C "$tmp" commit -qm "edit docs"
  sha_pr_docs="$(git -C "$tmp" rev-parse HEAD)"
  got="$(changed_for_pr "$tmp" "$sha_c" "$sha_pr_docs")"
  [[ "$got" == yes ]] || { echo "shipped Markdown PR expected yes, got: $got" >&2; return 1; }

  echo "void foo() {}" >> "$tmp/main/firmware.cpp"
  git -C "$tmp" add main/firmware.cpp
  git -C "$tmp" commit -qm "edit firmware"
  sha_pr_fw="$(git -C "$tmp" rev-parse HEAD)"
  got="$(changed_for_pr "$tmp" "$sha_c" "$sha_pr_fw")"
  [[ "$got" == yes ]] || { echo "firmware PR expected yes, got: $got" >&2; return 1; }

  got="$(changed_for_pr "$tmp" "bad-sha" "$sha_pr_fw" 2>/dev/null)"
  [[ "$got" == yes ]] || { echo "invalid PR SHA expected yes, got: $got" >&2; return 1; }

  # Dev channel relevance tests
  # 1. When dev/manifest.json does not exist yet on gh-pages, changed_since_dev falls back to release baseline
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_dev "$tmp" "$sha_b")"
  [[ "$got" == no ]] || {
    echo "dev fallback to published Release baseline expected no for docs B, got: $got" >&2; return 1;
  }

  # 2. Publish dev manifest on gh-pages at sha_c
  fake_live_dev="$tmp/fake-live-dev-manifest.json"
  git -C "$tmp" checkout -q gh-pages
  mkdir -p "$tmp/dev"
  write_pages_manifest "$tmp/dev/manifest.json" "1.0.1-dev.1" "$sha_c"
  git -C "$tmp" add dev/manifest.json dev/changelog.json
  git -C "$tmp" commit -qm "dev-pages-c"
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main
  write_pages_manifest "$fake_live_dev" "1.0.1-dev.1" "$sha_c"

  # Unshipped Markdown D is irrelevant for Dev, but C's earlier firmware still needs a Release.
  printf 'unshipped-d\n' >> "$tmp/README.md"
  git -C "$tmp" add README.md
  git -C "$tmp" commit -qm docs-d
  sha_d="$(git -C "$tmp" rev-parse HEAD)"
  got_rel="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$sha_d")"
  [[ "$got_rel" == yes ]] || {
    echo "docs-only D against published Release baseline expected yes (due to earlier firmware change in C), got: $got_rel" >&2; return 1;
  }
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" changed_since_dev "$tmp" "$sha_d")"
  [[ "$got" == no ]] || {
    echo "docs-only D against published dev baseline expected no, got: $got" >&2; return 1;
  }
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" GH_FAKE_CHANGELOG_MISMATCH=1 \
    changed_since_dev "$tmp" "$sha_d" 2>"$tmp/dev-metadata-mismatch.err")"
  [[ "$got" == yes ]] && grep -q "reconciliation stays open" "$tmp/dev-metadata-mismatch.err" || {
    echo "dev metadata mismatch did not keep reconciliation open: $got" >&2; return 1;
  }
  git -C "$tmp" checkout -q gh-pages
  cp "$tmp/dev/changelog.json" "$fakebin/held-dev-changelog.json"
  git -C "$tmp" rm -q dev/changelog.json
  git -C "$tmp" commit -qm dev-metadata-missing
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" \
    changed_since_dev "$tmp" "$sha_d" 2>"$tmp/dev-metadata-missing.err")"
  [[ "$got" == yes ]] && grep -q "reconciliation stays open" "$tmp/dev-metadata-missing.err" || {
    echo "missing dev metadata fell back to Release: $got" >&2; return 1;
  }
  git -C "$tmp" checkout -q gh-pages
  cp "$fakebin/held-dev-changelog.json" "$tmp/dev/changelog.json"
  git -C "$tmp" add dev/changelog.json
  git -C "$tmp" commit -qm dev-metadata-restored
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" changed_since_dev "$tmp" "$sha_d")"
  [[ "$got" == no ]] || {
    echo "restored dev metadata failed acceptance: $got" >&2; return 1;
  }

  # A dev channel whose manifest identity is current but whose served bytes do not match that
  # manifest is a known-broken publication: a docs-only push must NOT count it as a baseline and
  # skip the repair. It must not silently fall back to the Release baseline either, which would
  # only happen to say yes here; the answer has to be yes because of the broken channel itself.
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" GH_FAKE_DEV_BYTES_STALE=1 \
    changed_since_dev "$tmp" "$sha_d" 2>"$tmp/dev-stale.err")"
  [[ "$got" == yes ]] || {
    echo "dev channel with stale served bytes was accepted as a baseline: $got" >&2; return 1;
  }
  grep -q "reconciliation stays open" "$tmp/dev-stale.err" || {
    echo "stale dev bytes fell back to the Release baseline instead of failing closed" >&2; return 1;
  }

  # A dev channel whose manifest SHA differs from the branch manifest (even if sourceSha/version match)
  # must keep reconciliation open (return 3 -> yes) instead of falling back to Release baseline.
  printf ' ' >> "$fake_live_dev"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" \
    changed_since_dev "$tmp" "$sha_d" 2>"$tmp/dev-manifest-mismatch.err")"
  [[ "$got" == yes ]] || {
    echo "dev channel with mismatched manifest SHA was accepted as a baseline: $got" >&2; return 1;
  }
  grep -q "reconciliation stays open" "$tmp/dev-manifest-mismatch.err" || {
    echo "mismatched dev manifest SHA fell back to Release baseline instead of keeping reconciliation open" >&2; return 1;
  }
  write_pages_manifest "$fake_live_dev" "1.0.1-dev.1" "$sha_c"

  # Integration control: actual Pages builder output (multi-line JSON with final newline)
  # must be accepted by dev baseline (reconciled, changed_since_dev == no).
  local fixture_stage="$tmp/fixture_stage"
  mkdir -p "$fixture_stage"
  for tgt in esp32 esp32s3 esp32c3 esp32c6; do
    mkdir -p "$fixture_stage/$tgt"
    dd if=/dev/zero of="$fixture_stage/$tgt/bootloader.bin" bs=1 count=1024 status=none
    dd if=/dev/zero of="$fixture_stage/$tgt/partition-table.bin" bs=1 count=1024 status=none
    dd if=/dev/zero of="$fixture_stage/$tgt/tesla-key-esp32.bin" bs=1 count=4096 status=none
    "$GH_FAKE_REAL_PYTHON" -c 'import sys; open(sys.argv[1], "wb").write(b"\xff"*8192)' "$fixture_stage/$tgt/ota_data_initial.bin"
  done
  # The real assembler and its Git-based generator must see only this owned history.
  # A new object store, no remote and an explicit cwd exclude the operator's worktree/refs.
  local builder_repo="$tmp/actual-pages-repo" fixture_site builder_input
  git -c core.hooksPath=/dev/null clone --quiet --template= --no-local --no-hardlinks \
    --origin origin "$tmp" "$builder_repo"
  git -C "$builder_repo" remote remove origin
  [[ -d "$builder_repo/.git" && ! -L "$builder_repo/.git" \
      && ! -e "$builder_repo/.git/commondir" \
      && ! -e "$builder_repo/.git/objects/info/alternates" ]] || return 1
  [[ -z "$(git -C "$builder_repo" remote)" \
      && -z "$(git -C "$builder_repo" for-each-ref --format='%(refname)' refs/remotes/)" \
      && "$(git -C "$builder_repo" rev-parse --verify "${sha_c}^{commit}")" == "$sha_c" ]] || return 1
  rm -rf -- "$builder_repo/scripts" "$builder_repo/docs"
  mkdir -p "$builder_repo/scripts"
  for builder_input in build-pages.sh verify-vendored-esptool-js.sh check-pages-manifest.py check-otadata-contract.py generate-ota-changelog.py; do
    cp -p "$contract_root/scripts/$builder_input" "$builder_repo/scripts/$builder_input"
  done
  cp -pR "$contract_root/docs" "$builder_repo/docs"
  cp -p "$contract_root/LICENSE" "$builder_repo/LICENSE"
  fixture_site="$builder_repo/_site"
  (
    cd "$builder_repo"
    [[ "$PWD" -ef "$builder_repo" ]] || exit 1
    FIRMWARE_STAGE_DIR="$fixture_stage" ./scripts/build-pages.sh "$fixture_site" "1.0.1-dev.1" "$sha_c" >/dev/null
  )
  cp "$fixture_site/manifest.json" "$fake_live_dev"
  git -C "$tmp" checkout -q gh-pages
  cp "$fixture_site/manifest.json" "$tmp/dev/manifest.json"
  cp "$fixture_site/changelog.json" "$tmp/dev/changelog.json"
  git -C "$tmp" add dev/manifest.json dev/changelog.json
  git -C "$tmp" commit -qm "dev-pages-actual-builder"
  git -C "$tmp" push -q origin gh-pages
  git -C "$tmp" checkout -q main
  rm -rf -- "$builder_repo"

  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" changed_since_dev "$tmp" "$sha_d")"
  [[ "$got" == no ]] || {
    echo "actual generated Pages manifest with trailing newline rejected by dev baseline: $got" >&2; return 1;
  }

  # 3. New firmware commit E on main should be relevant for dev
  mkdir -p "$tmp/main"
  printf 'firmware-e\n' > "$tmp/main/firmware.cpp"
  git -C "$tmp" add main/firmware.cpp
  git -C "$tmp" commit -qm firmware-e
  sha_e="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" changed_since_dev "$tmp" "$sha_e")"
  [[ "$got" == yes ]] || {
    echo "firmware commit E against published dev baseline expected yes, got: $got" >&2; return 1;
  }

  # 4. Changes to check-dev-pages.py and generate-ota-changelog.py must be relevant
  printf '# check-dev-pages\n' > "$tmp/scripts/check-dev-pages.py"
  git -C "$tmp" add scripts/check-dev-pages.py
  git -C "$tmp" commit -qm check-dev-pages-contract
  sha_contract_dev="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" changed_since_dev "$tmp" "$sha_contract_dev")"
  [[ "$got" == yes ]] || {
    echo "check-dev-pages script change was not dev-relevant: $got" >&2; return 1;
  }

  printf '# generate-ota-changelog\n' > "$tmp/scripts/generate-ota-changelog.py"
  git -C "$tmp" add scripts/generate-ota-changelog.py
  git -C "$tmp" commit -qm generate-ota-changelog-contract
  sha_contract_cl="$(git -C "$tmp" rev-parse HEAD)"
  got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
    GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" changed_since_dev "$tmp" "$sha_contract_cl")"
  [[ "$got" == yes ]] || {
    echo "generate-ota-changelog script change was not dev-relevant: $got" >&2; return 1;
  }

  # Each classifier sees one actual operation against its immediate published baseline.
  # Re-publish only inside this owned inert fixture; no old image is relabeled in production.
  local markdown_counter=2 markdown_case markdown_base markdown_head markdown_version markdown_expected
  for markdown_case in add edit rename delete unicode-name newline-name dependency-helper dependency-manifest dependency-cmake unshipped-root unshipped-nested unshipped-hidden unshipped-newline; do
    markdown_base="$(git -C "$tmp" rev-parse HEAD)"
    markdown_version="1.0.$markdown_counter"
    markdown_counter=$((markdown_counter + 1))
    git -C "$tmp" tag "v$markdown_version" "$markdown_base"
    git -C "$tmp" push -q origin main --tags
    git -C "$tmp" checkout -q gh-pages
    write_pages_manifest "$tmp/manifest.json" "$markdown_version" "$markdown_base"
    write_pages_manifest "$tmp/dev/manifest.json" "$markdown_version-dev.1" "$markdown_base"
    if [[ "$markdown_case" == add ]]; then
      printf '\n \n' >> "$tmp/manifest.json"
      cp "$tmp/manifest.json" "$tmp/root-raw-manifest-expected.json"
    fi
    git -C "$tmp" add manifest.json changelog.json dev/manifest.json dev/changelog.json
    git -C "$tmp" commit -qm "metadata-baseline-$markdown_case"
    git -C "$tmp" push -q origin gh-pages
    git -C "$tmp" checkout -q main
    write_fake_release "$fake_release" "$markdown_version" "$markdown_base"
    write_pages_manifest "$fake_live" "$markdown_version" "$markdown_base"
    write_pages_manifest "$fake_live_dev" "$markdown_version-dev.1" "$markdown_base"
    got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
      GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$markdown_base")"
    [[ "$got" == no ]] || { echo "settled root metadata baseline was not accepted: $markdown_case" >&2; return 1; }
    if [[ "$markdown_case" == add ]]; then
      got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
        GH_FAKE_LIVE="$fake_live" GH_FAKE_EXPECTED_ROOT_MANIFEST="$tmp/root-raw-manifest-expected.json" \
        changed_since_release "$tmp" "$markdown_base")"
      [[ "$got" == no ]] || { echo "root baseline did not retain exact branch manifest trailing bytes: $got" >&2; return 1; }
    fi
    got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
      GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" changed_since_dev "$tmp" "$markdown_base")"
    [[ "$got" == no ]] || { echo "settled dev metadata baseline was not accepted: $markdown_case" >&2; return 1; }
    [[ "$(changed_for_pr "$tmp" "$markdown_base" "$markdown_base")" == no ]] || return 1
    markdown_expected=yes
    case "$markdown_case" in
      add)
        printf 'shipped addition\n' > "$tmp/docs/relevance-fixture.md"
        git -C "$tmp" add docs/relevance-fixture.md
        ;;
      edit)
        printf 'shipped edit\n' >> "$tmp/docs/relevance-fixture.md"
        git -C "$tmp" add docs/relevance-fixture.md
        ;;
      rename) git -C "$tmp" mv docs/relevance-fixture.md docs/relevance-renamed.md ;;
      delete) git -C "$tmp" rm -q docs/relevance-renamed.md ;;
      unicode-name)
        printf 'shipped Unicode path\n' > "$tmp/docs/API–guide.md"
        git -C "$tmp" add docs/API–guide.md
        ;;
      newline-name)
        printf 'shipped newline path\n' > "$tmp/docs/"$'API\nguide.md'
        git -C "$tmp" add "docs/"$'API\nguide.md'
        ;;
      dependency-helper|dependency-manifest|dependency-cmake)
        # Each production dependency owner must independently trigger all three selectors.
        case "$markdown_case" in
          dependency-helper) contract_path=scripts/dependency_source.py ;;
          dependency-manifest) contract_path=scripts/nanopb-source.json ;;
          dependency-cmake) contract_path=cmake/admit-dependency-sources.cmake ;;
        esac
        mkdir -p "$tmp/${contract_path%/*}"
        printf 'changed dependency source admission\n' > "$tmp/$contract_path"
        git -C "$tmp" add "$contract_path"
        ;;
      unshipped-root)
        printf 'unshipped edit\n' >> "$tmp/README.md"
        git -C "$tmp" add README.md
        markdown_expected=no
        ;;
      unshipped-nested)
        mkdir -p "$tmp/docs/adr"
        printf 'unshipped nested addition\n' > "$tmp/docs/adr/relevance-fixture.md"
        git -C "$tmp" add docs/adr/relevance-fixture.md
        markdown_expected=no
        ;;
      unshipped-hidden)
        printf 'not copied by the Pages glob\n' > "$tmp/docs/.hidden-guide.md"
        git -C "$tmp" add docs/.hidden-guide.md
        markdown_expected=no
        ;;
      unshipped-newline)
        printf 'unshipped path containing a relevant-looking line\n' > "$tmp/"$'README\nmain.md'
        git -C "$tmp" add $'README\nmain.md'
        markdown_expected=no
        ;;
    esac
    git -C "$tmp" commit -qm "markdown-$markdown_case"
    markdown_head="$(git -C "$tmp" rev-parse HEAD)"
    got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
      GH_FAKE_LIVE="$fake_live" changed_since_release "$tmp" "$markdown_head")"
    [[ "$got" == "$markdown_expected" ]] || { echo "root Markdown relevance wrong: $markdown_case/$got" >&2; return 1; }
    got="$(PATH="$fakebin:$PATH" GITHUB_REPOSITORY=owner/repo GH_FAKE_RELEASE="$fake_release" \
      GH_FAKE_LIVE="$fake_live" GH_FAKE_LIVE_DEV="$fake_live_dev" changed_since_dev "$tmp" "$markdown_head")"
    [[ "$got" == "$markdown_expected" ]] || { echo "dev Markdown relevance wrong: $markdown_case/$got" >&2; return 1; }
    got="$(changed_for_pr "$tmp" "$markdown_base" "$markdown_head")"
    [[ "$got" == "$markdown_expected" ]] || { echo "PR Markdown relevance wrong: $markdown_case/$got" >&2; return 1; }
  done
  echo "release relevance self-test: PASS"
}

repo_root="$contract_root"
case "${1:-}" in
  --changed)
    [[ $# -eq 2 ]] || { echo "usage: $0 --changed CURRENT_SHA | --changed-dev CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2; }
    changed_since_release "$repo_root" "$2"
    ;;
  --changed-dev)
    [[ $# -eq 2 ]] || { echo "usage: $0 --changed CURRENT_SHA | --changed-dev CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2; }
    changed_since_dev "$repo_root" "$2"
    ;;
  --changed-pr)
    [[ $# -eq 3 ]] || { echo "usage: $0 --changed CURRENT_SHA | --changed-dev CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2; }
    changed_for_pr "$repo_root" "$2" "$3"
    ;;
  --self-test)
    [[ $# -eq 1 ]] || { echo "usage: $0 --changed CURRENT_SHA | --changed-dev CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2; }
    self_test
    ;;
  *) echo "usage: $0 --changed CURRENT_SHA | --changed-dev CURRENT_SHA | --changed-pr BASE_SHA HEAD_SHA | --self-test" >&2; exit 2 ;;
esac
