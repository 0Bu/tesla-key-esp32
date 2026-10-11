#!/usr/bin/env bash
# Run a host check with Python >= 3.11 without changing the user's installation or PATH.
set -euo pipefail

[ "$#" -gt 0 ] || { echo "with-host-python: command required" >&2; exit 2; }
supports_host_checks() {
    "$1" -c 'import sys; raise SystemExit(sys.version_info < (3, 11))' >/dev/null 2>&1
}
if command -v python3 >/dev/null 2>&1 && supports_host_checks python3; then
    exec "$@"
fi
host_python=""
for candidate in python3.14 python3.13 python3.12 python3.11; do
    candidate_path="$(command -v "$candidate" || true)"
    if [ -n "$candidate_path" ] && supports_host_checks "$candidate_path"; then
        host_python="$candidate_path"
        break
    fi
done
[ -n "$host_python" ] || {
    echo "with-host-python: Python >= 3.11 is required for repository host checks" >&2
    exit 2
}
python_shim="$(mktemp -d "${TMPDIR:-/tmp}/tesla-host-python.XXXXXX")"
trap 'rm -rf "$python_shim"' EXIT
ln -s "$host_python" "$python_shim/python3"
PATH="$python_shim:$PATH" "$@"
