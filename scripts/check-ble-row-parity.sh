#!/usr/bin/env bash
# Verify the web UI's BLE-row decision (the BLE_ROW region of main/www/app.js) against the C++
# presenter it mirrors (tk::ble::decide in main/logic/ble_row.hpp): compile a tiny host dumper,
# emit golden decision vectors for an exhaustive input sweep, and have the JavaScript re-decide
# the same inputs and diff. Fails if the browser and the firmware presenter disagree — so "the
# row renders what the host-tested rules say" is checked by CI, not by hand.
#
# Needs a C++17 compiler + node (both present in the CI logic-test job — ubuntu-latest ships
# Node). Run directly, or automatically at the end of scripts/run-mock-tests.sh.
set -euo pipefail

cd "$(dirname "$0")/.."

OUT=build_mock          # matches .gitignore (/build_mock/)
mkdir -p "$OUT"

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
    if   command -v g++     >/dev/null 2>&1; then CXX=g++
    elif command -v clang++ >/dev/null 2>&1; then CXX=clang++
    else echo "check-ble-row-parity: need a C++17 compiler (g++/clang++)" >&2; exit 1
    fi
fi

"$CXX" -std=c++17 -Wall -Wextra -Werror -Imain \
    -o "$OUT/ble_row_golden_dump" test/ble_row_golden_dump.cpp
"$OUT/ble_row_golden_dump" > "$OUT/ble_row_golden.tsv"

node tools/ble_row_parity.js "$OUT/ble_row_golden.tsv"
