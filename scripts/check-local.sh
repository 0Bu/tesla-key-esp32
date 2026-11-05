#!/usr/bin/env bash
# Complete local handoff, without publication, signing authority or device contact.
set -euo pipefail
unset OTA_SIGNING_KEY OTA_SIGNING_KEY_FILE
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
firmware=0
case "${1:-}" in
  "") [ "$#" -eq 0 ] || exit 2 ;;
  --firmware) [ "$#" -eq 1 ] || exit 2; firmware=1 ;;
  --help) echo 'usage: scripts/check-local.sh [--firmware]'; exit 0 ;;
  *) echo 'usage: scripts/check-local.sh [--firmware]' >&2; exit 2 ;;
esac
if ! python3 -c 'import sys; raise SystemExit(sys.version_info < (3, 11))' >/dev/null 2>&1; then
  exec scripts/with-host-python.sh bash "$0" "$@"
fi
tools/agent-config/selftest.sh
scripts/repo-lint.sh
python3 scripts/run-local-checks.py --require-all
scripts/test-tesla-ble-harness.sh --require-all
scripts/test-pr-gates.sh
if [ "$(uname -s)" = Linux ]; then
  scripts/run-sanitizer-tests.sh --self-test
  scripts/run-sanitizer-tests.sh
else
  # Keep Linux detector/runtime evidence separate from native host evidence; use the pinned image.
  scripts/idf-docker.sh bash -c './scripts/run-sanitizer-tests.sh --self-test && ./scripts/run-sanitizer-tests.sh'
fi
if [ "$firmware" = 1 ]; then
  scripts/idf-docker.sh ./scripts/ci-build-verify.sh "$(cat version.txt)" "$(git rev-parse HEAD)"
fi
git diff --check
echo 'local-handoff: PASS (local checks only; no remote CI, publication or device acceptance)'
