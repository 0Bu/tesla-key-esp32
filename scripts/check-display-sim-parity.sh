#!/usr/bin/env bash
# Verify tools/display_sim.py's decide() against the C++ display presenter (tk::display::compose
# in main/logic/display_model.hpp): compile a tiny host dumper, emit golden decision vectors,
# and have the sim re-decide the same inputs and diff the result. Fails if the pixel sim and the
# firmware presenter disagree — so "the sim mirrors display.cpp 1:1" is checked by CI, not by
# hand. Needs a C++17 compiler + python3 (both present in the CI logic-test job). Run directly,
# or automatically at the end of scripts/run-mock-tests.sh.
set -euo pipefail

cd "$(dirname "$0")/.."

OUT=build_mock          # matches .gitignore (/build_mock/)
mkdir -p "$OUT"

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
    if   command -v g++     >/dev/null 2>&1; then CXX=g++
    elif command -v clang++ >/dev/null 2>&1; then CXX=clang++
    else echo "check-display-sim-parity: need a C++17 compiler (g++/clang++)" >&2; exit 1
    fi
fi

"$CXX" -std=c++17 -Wall -Wextra -Werror -Imain \
    -o "$OUT/display_golden_dump" test/display_golden_dump.cpp
"$OUT/display_golden_dump" > "$OUT/display_golden.tsv"

python3 tools/display_sim.py parity "$OUT/display_golden.tsv"
