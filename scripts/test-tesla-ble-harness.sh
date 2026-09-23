#!/usr/bin/env bash
# Host-side integration harness test against the real yoziru/tesla-ble v5.2.0 library,
# Nanopb, and Mbed TLS.
#
# Runs the production helpers from main/logic/ (not copies) against the real library:
# - B1: tk::build_ble_tx_frame / tk::is_well_formed_ble_frame (the drive_command_runner_() TX path)
#       with real builders, decoded vehicle-side like vehicle-command's ble.go
# - B2: tk::regenerate_private_key (behind regenerate_key_native_()): round trip, 2048 B PEM
#       export, fail-closed rollback
# - H1: CommandRunner/BleDispatcher routing contract for foreign-UUID CarServer responses
#
# Usage: ./scripts/test-tesla-ble-harness.sh [--clean]

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build_harness}"
CACHE_DIR="${CACHE_DIR:-$BUILD_DIR/_cache}"

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$BUILD_DIR"
    exit 0
fi

REQUIRE_ALL=0
for arg in "$@"; do
    if [ "$arg" = "--require-all" ]; then
        REQUIRE_ALL=1
    fi
done

mkdir -p "$BUILD_DIR/obj/mbedtls" "$BUILD_DIR/obj/nanopb" "$BUILD_DIR/obj/tb" "$CACHE_DIR"

clone_or_fail() {
    local name="$1" branch="$2" url="$3" dest="$4"
    if [ ! -d "$dest" ]; then
        echo "[harness] Cloning $name ($branch)..."
        if ! git clone --depth 1 --branch "$branch" "$url" "$dest"; then
            if [ "$REQUIRE_ALL" = 1 ] || [ "${CI:-}" = "true" ]; then
                echo "[harness] ERROR: failed to clone $name in fail-closed mode" >&2
                exit 1
            else
                echo "[harness] offline / network clone failed for $name — skipping integration harness"
                exit 0
            fi
        fi
    fi
}

# 1. Locate or clone dependencies
EXPECTED_TB_VER="$(ruby -ryaml -e 'puts YAML.load_file("'"$ROOT_DIR/main/idf_component.yml"'")["dependencies"]["yoziru/tesla-ble"]["version"]' 2>/dev/null || awk '/dependencies:/{in_deps=1} in_deps && /yoziru\/tesla-ble:/{in_tb=1} in_tb && /^[[:space:]]*version:/{sub(/.*version:[[:space:]]*"?/, ""); sub(/".*/, ""); print; exit}' "$ROOT_DIR/main/idf_component.yml")"
[ -n "$EXPECTED_TB_VER" ] || { echo "[harness] ERROR: cannot read yoziru/tesla-ble version from main/idf_component.yml" >&2; exit 1; }

if [ -d "$ROOT_DIR/managed_components/yoziru__tesla-ble" ]; then
    TB_DIR="$ROOT_DIR/managed_components/yoziru__tesla-ble"
    if [ ! -f "$TB_DIR/idf_component.yml" ]; then
        echo "[harness] ERROR: $TB_DIR exists but idf_component.yml is missing" >&2
        exit 1
    fi
    LOCAL_VER="$(sed -n 's/^version:[[:space:]]*"\([^"]*\)".*/\1/p' "$TB_DIR/idf_component.yml" 2>/dev/null || true)"
    if [ -z "$LOCAL_VER" ]; then
        echo "[harness] ERROR: cannot read version from $TB_DIR/idf_component.yml" >&2
        exit 1
    fi
    if [ "$LOCAL_VER" != "${EXPECTED_TB_VER#v}" ] && [ "$LOCAL_VER" != "$EXPECTED_TB_VER" ]; then
        echo "[harness] ERROR: managed_components/yoziru__tesla-ble ($LOCAL_VER) does not match expected version ($EXPECTED_TB_VER)" >&2
        exit 1
    fi
else
    TB_DIR="$CACHE_DIR/tb"
    if [ -d "$TB_DIR" ]; then
        TB_CACHED_VER=""
        if [ -f "$TB_DIR/idf_component.yml" ]; then
            TB_CACHED_VER="$(sed -n 's/^version:[[:space:]]*"\([^"]*\)".*/\1/p' "$TB_DIR/idf_component.yml" 2>/dev/null || true)"
        fi
        if [ -z "$TB_CACHED_VER" ] || { [ "$TB_CACHED_VER" != "${EXPECTED_TB_VER#v}" ] && [ "$TB_CACHED_VER" != "$EXPECTED_TB_VER" ]; }; then
            echo "[harness] ERROR: cached tesla-ble in $TB_DIR has version '$TB_CACHED_VER', expected pinned '$EXPECTED_TB_VER'" >&2
            exit 1
        fi
    else
        clone_or_fail "yoziru/tesla-ble" "$EXPECTED_TB_VER" "https://github.com/yoziru/tesla-ble.git" "$TB_DIR"
    fi
fi

echo "[harness] Applying repository patches to tesla-ble ($TB_DIR)..."
TESLA_BLE_COMPONENT_DIR="$TB_DIR" "$ROOT_DIR/scripts/apply-tesla-ble-patches.sh"

NANOPB_DIR="$ROOT_DIR/build/_deps/nanopb-src"
if [ ! -d "$NANOPB_DIR" ]; then
    NANOPB_DIR="$CACHE_DIR/nanopb"
    clone_or_fail "nanopb" "0.4.9.1" "https://github.com/nanopb/nanopb.git" "$NANOPB_DIR"
fi

MBEDTLS_DIR="$CACHE_DIR/mbedtls"
clone_or_fail "mbedtls" "mbedtls-3.6.6-idf" "https://github.com/espressif/mbedtls.git" "$MBEDTLS_DIR"

CC="${CC:-clang}"
CXX="${CXX:-clang++}"
if ! command -v "$CC" >/dev/null 2>&1; then
    CC=gcc
fi
if ! command -v "$CXX" >/dev/null 2>&1; then
    CXX=g++
fi

echo "[harness] Compiling Mbed TLS..."
for f in "$MBEDTLS_DIR"/library/*.c; do
    obj="$BUILD_DIR/obj/mbedtls/$(basename "$f" .c).o"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        "$CC" -O2 -w -c "$f" -I "$MBEDTLS_DIR/include" -I "$MBEDTLS_DIR/library" -o "$obj"
    fi
done
ar rcs "$BUILD_DIR/libmbedtls.a" "$BUILD_DIR"/obj/mbedtls/*.o

echo "[harness] Compiling Nanopb..."
INC="-I $TB_DIR/include -I $TB_DIR/generated/include -I $NANOPB_DIR -I $MBEDTLS_DIR/include -I $ROOT_DIR/main"
for f in "$NANOPB_DIR"/pb_common.c "$NANOPB_DIR"/pb_decode.c "$NANOPB_DIR"/pb_encode.c; do
    obj="$BUILD_DIR/obj/nanopb/$(basename "$f" .c).o"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        "$CC" -O2 -w -c "$f" -I "$NANOPB_DIR" -o "$obj"
    fi
done
if [ -f "$BUILD_DIR/obj/tb/.version" ]; then
    CACHED_OBJ_VER="$(cat "$BUILD_DIR/obj/tb/.version" 2>/dev/null || true)"
    if [ "$CACHED_OBJ_VER" != "$EXPECTED_TB_VER" ]; then
        echo "[harness] tesla-ble version changed ($CACHED_OBJ_VER -> $EXPECTED_TB_VER); invalidating object cache..."
        rm -rf "$BUILD_DIR/obj/tb"
        mkdir -p "$BUILD_DIR/obj/tb"
    fi
fi
echo "$EXPECTED_TB_VER" > "$BUILD_DIR/obj/tb/.version"

echo "[harness] Compiling tesla-ble protobuf descriptors..."
while IFS= read -r -d '' f; do
    rel="${f#"$TB_DIR/generated/src/"}"
    flat_name="${rel//\//_}"
    flat_name="${flat_name%.c}.o"
    obj="$BUILD_DIR/obj/tb/$flat_name"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        "$CC" -O2 -w -c "$f" $INC -o "$obj"
    fi
done < <(find "$TB_DIR/generated/src" -name '*.pb.c' -print0)

echo "[harness] Compiling tesla-ble core sources..."
for f in "$TB_DIR"/src/*.cpp; do
    if [ -f "$f" ] && [ "$(basename "$f")" != "vehicle.cpp" ]; then
        obj="$BUILD_DIR/obj/tb/$(basename "$f" .cpp).o"
        if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
            "$CXX" -std=c++17 -O2 -w -c "$f" $INC -o "$obj"
        fi
    fi
done

ar rcs "$BUILD_DIR/libtb.a" "$BUILD_DIR"/obj/tb/*.o "$BUILD_DIR"/obj/nanopb/*.o

echo "[harness] Compiling test_tesla_ble_harness..."
"$CXX" -std=c++17 -O2 -Wall -Wextra \
    "$ROOT_DIR/test/test_tesla_ble_harness.cpp" \
    $INC "$BUILD_DIR/libtb.a" "$BUILD_DIR/libmbedtls.a" \
    -o "$BUILD_DIR/test_tesla_ble_harness"

echo "[harness] Executing integration harness..."
"$BUILD_DIR/test_tesla_ble_harness"
echo "[harness] PASS: tesla-ble integration harness verified successfully"
