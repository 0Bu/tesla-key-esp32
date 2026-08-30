#!/usr/bin/env bash
# Deterministic, bounded property-fuzz smoke for IDF-free parsers and codecs. This is deliberately
# not an open-ended libFuzzer job: every PR gets the same reproducible corpus in seconds, while the
# sanitizer job compiles and executes this same driver with ASan/UBSan/LSan.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
    if command -v g++ >/dev/null 2>&1; then CXX=g++
    elif command -v clang++ >/dev/null 2>&1; then CXX=clang++
    else
        echo "fuzz-smoke: a C++17 compiler is required" >&2
        exit 2
    fi
fi

iterations="${FUZZ_SMOKE_ITERATIONS:-20000}"
case "$iterations" in
    ''|*[!0-9]*) echo "fuzz-smoke: FUZZ_SMOKE_ITERATIONS must be an integer" >&2; exit 2 ;;
esac
if [ "$iterations" -lt 1 ] || [ "$iterations" -gt 100000 ]; then
    echo "fuzz-smoke: FUZZ_SMOKE_ITERATIONS must be in 1..100000" >&2
    exit 2
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/tesla-key-fuzz.XXXXXX")"
trap 'rm -rf "$work"' EXIT

"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -Imain \
    test/fuzz_smoke.cpp -o "$work/fuzz_smoke"
"$work/fuzz_smoke" --seed 0x5445534c414b4559 --iterations "$iterations"
