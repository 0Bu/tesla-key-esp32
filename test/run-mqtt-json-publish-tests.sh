#!/usr/bin/env bash
# Compile the MQTT build/print/publish gate against the exact cJSON source shipped by pinned IDF.
set -euo pipefail

: "${IDF_PATH:?run inside the pinned ESP-IDF environment}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cjson_dir="$IDF_PATH/components/json/cJSON"
cjson_source="$cjson_dir/cJSON.c"
cjson_header="$cjson_dir/cJSON.h"

if [ ! -f "$cjson_source" ] || [ ! -f "$cjson_header" ]; then
    echo "MQTT JSON gate: pinned ESP-IDF cJSON source/header not found under $cjson_dir" >&2
    exit 1
fi

idf_tag="$(git -C "$IDF_PATH" describe --tags --exact-match HEAD 2>/dev/null || true)"
if [ "$idf_tag" != "v5.5.5" ]; then
    echo "MQTT JSON gate: expected pinned ESP-IDF v5.5.5, got ${idf_tag:-unknown}" >&2
    exit 1
fi

if grep -En 'JsonBuilder|cJSON_(Create|Add|Print)' "$repo_root/main/mqtt_ha.cpp"; then
    echo "MQTT JSON gate: mqtt_ha.cpp bypasses the shared production payload emitters" >&2
    exit 1
fi

required_emitters=(
    build_discovery_payload
    build_charge_payload
    build_climate_payload
    build_drive_payload
    build_tires_payload
    build_closures_payload
    build_vehicle_payload
    build_device_payload
    mqtt_run_discovery_round
    mqtt_run_state_round
)
for emitter in "${required_emitters[@]}"; do
    if ! grep -Fq "$emitter" "$repo_root/main/mqtt_ha.cpp"; then
        echo "MQTT JSON gate: mqtt_ha.cpp does not delegate to $emitter" >&2
        exit 1
    fi
done

work="$(mktemp -d "${TMPDIR:-/tmp}/tesla-mqtt-json.XXXXXX")"
trap 'rm -rf "$work"' EXIT

cc="${CC:-gcc}"
cxx="${CXX:-g++}"
sanitize_flags=()
if [ "${MQTT_JSON_SANITIZE:-0}" = 1 ]; then
    sanitize_flags=(-fsanitize=address,undefined,leak -fno-sanitize-recover=all)
fi

"$cc" -std=c11 -Wall -Wextra -Werror "${sanitize_flags[@]}" -I"$cjson_dir" \
    -c "$cjson_source" -o "$work/cJSON.o"
"$cxx" -std=c++17 -Wall -Wextra -Werror "${sanitize_flags[@]}" \
    -I"$repo_root/main" -I"$cjson_dir" \
    "$repo_root/test/test_mqtt_json_publish.cpp" "$work/cJSON.o" -lm \
    -o "$work/test_mqtt_json_publish"
"$work/test_mqtt_json_publish"
