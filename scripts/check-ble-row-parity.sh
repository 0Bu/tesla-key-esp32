#!/usr/bin/env bash
# Compare shipped browser BLE decisions, SoC colours and RSSI glyph counts with native helpers.
# The host dumper produces both input/vector sets; Node evaluates the marked shipped JS and
# the actual signal SVG builder. This proves the covered presenter decisions, not pixels or
# physical signal accuracy. The strict mock runner invokes this gate.
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

"$OUT/ble_row_golden_dump" --web-presenters > "$OUT/web_presenter_golden.tsv"
node tools/ble_row_parity.js "$OUT/ble_row_golden.tsv" "$OUT/web_presenter_golden.tsv"
