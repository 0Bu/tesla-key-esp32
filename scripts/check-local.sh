#!/usr/bin/env bash
# Complete local handoff, without publication, signing authority or device contact.
set -euo pipefail
unset OTA_SIGNING_KEY OTA_SIGNING_KEY_FILE
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
firmware=0
candidate=0
usage='usage: scripts/check-local.sh [--candidate] [--firmware]'
for option in "$@"; do
  case "$option" in
    --candidate) candidate=1 ;;
    --firmware) firmware=1 ;;
    --help) echo "$usage"; exit 0 ;;
    *) echo "$usage" >&2; exit 2 ;;
  esac
done
if ! python3 -c 'import sys; raise SystemExit(sys.version_info < (3, 11))' >/dev/null 2>&1; then
  exec scripts/with-host-python.sh bash "$0" "$@"
fi
head="$(git rev-parse HEAD)"
if [ "$candidate" = 1 ]; then
  state="$(git -c core.fsmonitor=false status --porcelain)"
  [ -z "$state" ] || { echo 'local-handoff: candidate requires a clean committed tree' >&2; exit 1; }
fi

check_strict_host() {
  python3 scripts/run-local-checks.py --require-all
}

check_protocol_and_policy() {
  tools/agent-config/selftest.sh
  scripts/repo-lint.sh
  scripts/verify-vendored-esptool-js.sh --source
  scripts/test-tesla-ble-harness.sh --require-all
  scripts/ci-build-all.sh --self-test
  python3 scripts/check-sdkconfig-defaults.py --self-test
  python3 tools/agent-hooks/agent_hook.py build-efficiency --self-test
  test "$(scripts/idf-version.sh)" = v6.1
  scripts/test-pr-gates.sh
}

# shellcheck source=/dev/null
. tools/agent-hooks/parallel-checks.sh
run_parallel_checks check_strict_host check_protocol_and_policy

# Run compiler-heavy sanitizer and firmware work after the timed host gates.
if [ "$(uname -s)" = Linux ]; then
  scripts/run-sanitizer-tests.sh --self-test
  scripts/run-sanitizer-tests.sh
else
  scripts/idf-docker.sh bash -c './scripts/run-sanitizer-tests.sh --self-test && ./scripts/run-sanitizer-tests.sh'
fi
if [ "$firmware" = 1 ]; then
  scripts/idf-docker.sh ./scripts/ci-build-verify.sh "$(cat version.txt)" "$head"
fi
git diff --check
if [ "$candidate" = 1 ]; then
  state="$(git -c core.fsmonitor=false status --porcelain)"
  [ -z "$state" ] && [ "$(git rev-parse HEAD)" = "$head" ] || {
    echo 'local-handoff: candidate changed during verification' >&2
    exit 1
  }
  echo "local-handoff: verified candidate $head (firmware=$firmware)"
fi
echo 'local-handoff: PASS (local checks only; no remote CI, publication or device acceptance)'
