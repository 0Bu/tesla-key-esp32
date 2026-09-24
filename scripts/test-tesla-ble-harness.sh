#!/usr/bin/env bash
# Host-side integration harness test against the real yoziru/tesla-ble v5.2.0 library (with the
# repository patch series, including the PSA port), Nanopb, and the exact Espressif Mbed TLS 4.1 /
# TF-PSA-Crypto commit that the pinned ESP-IDF v6.1 builds into the firmware.
#
# Runs the production helpers from main/logic/ (not copies) against the real library:
# - B1: tk::build_ble_tx_frame / tk::is_well_formed_ble_frame (the drive_command_runner_() TX path)
#       with real builders, decoded vehicle-side like vehicle-command's ble.go
# - B2: tk::regenerate_private_key (behind regenerate_key_native_()): round trip, 2048 B PEM
#       export, fail-closed rollback
# - H1: CommandRunner/BleDispatcher routing contract for foreign-UUID CarServer responses
# - V1: protocol-vector known answers (vehicle-command protocol.md) through the patched PSA crypto
#       bindings: P-256 key import, ECDH session key, session-info HMAC, AES-GCM, VIN BLE name,
#       PEM round trip and the firmware key-fingerprint derivation
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

mkdir -p "$BUILD_DIR/obj/nanopb" "$BUILD_DIR/obj/tb" "$CACHE_DIR"

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

# Fetch one exact commit (a moving branch would let the host crypto drift from the firmware's).
fetch_commit_or_fail() {
    local name="$1" commit="$2" url="$3" dest="$4"
    if [ -d "$dest" ] && [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
        return 0
    fi
    rm -rf "$dest"
    echo "[harness] Fetching $name ($commit)..."
    if ! { git init -q "$dest" &&
           git -C "$dest" fetch -q --depth 1 "$url" "$commit" &&
           git -C "$dest" checkout -q FETCH_HEAD &&
           [ "$(git -C "$dest" rev-parse HEAD)" = "$commit" ]; }; then
        rm -rf "$dest"
        if [ "$REQUIRE_ALL" = 1 ] || [ "${CI:-}" = "true" ]; then
            echo "[harness] ERROR: failed to fetch $name in fail-closed mode" >&2
            exit 1
        fi
        echo "[harness] offline / network fetch failed for $name — skipping integration harness"
        exit 0
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

# The components/mbedtls/mbedtls submodule commit of ESP-IDF v6.1 (Mbed TLS 4.1.0 + TF-PSA-Crypto
# 1.1.0, Espressif fork). Keep it equal to the pinned toolchain's submodule on every IDF bump.
MBEDTLS_REF="a2b32072ea898afc1ed5b6caf6931e36028c91d6"
MBEDTLS_DIR="$CACHE_DIR/mbedtls-$MBEDTLS_REF"
fetch_commit_or_fail "espressif/mbedtls" "$MBEDTLS_REF" "https://github.com/espressif/mbedtls.git" "$MBEDTLS_DIR"

CC="${CC:-clang}"
CXX="${CXX:-clang++}"
if ! command -v "$CC" >/dev/null 2>&1; then
    CC=gcc
fi
if ! command -v "$CXX" >/dev/null 2>&1; then
    CXX=g++
fi

# Espressif's fork commits its generated sources (GEN_FILES=OFF) and includes ESP-IDF's public
# mbedtls/bignum.h and ecp.h wrappers from its builtin drivers; test/stubs/mbedtls-idf stands in
# for those two ESP-IDF port headers on the host.
command -v cmake >/dev/null 2>&1 || { echo "[harness] ERROR: cmake is required to build Mbed TLS 4" >&2; exit 1; }
MBEDTLS_BUILD="$BUILD_DIR/mbedtls-$MBEDTLS_REF"
if [ ! -f "$MBEDTLS_BUILD/tf-psa-crypto/core/libtfpsacrypto.a" ]; then
    echo "[harness] Building Mbed TLS 4 / TF-PSA-Crypto..."
    rm -rf "$MBEDTLS_BUILD"
    cmake -S "$MBEDTLS_DIR" -B "$MBEDTLS_BUILD" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_C_FLAGS="-I $ROOT_DIR/test/stubs/mbedtls-idf" \
        -DGEN_FILES=OFF -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
        -DMBEDTLS_FATAL_WARNINGS=OFF -DTF_PSA_CRYPTO_FATAL_WARNINGS=OFF >/dev/null
    cmake --build "$MBEDTLS_BUILD" --target tfpsacrypto -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" >/dev/null
fi
MBEDTLS_LIB="$MBEDTLS_BUILD/tf-psa-crypto/core/libtfpsacrypto.a"
MBEDTLS_INC="-I $MBEDTLS_DIR/include -I $MBEDTLS_DIR/tf-psa-crypto/include -I $MBEDTLS_DIR/tf-psa-crypto/drivers/builtin/include"

echo "[harness] Compiling Nanopb..."
INC="-I $TB_DIR/include -I $TB_DIR/generated/include -I $NANOPB_DIR $MBEDTLS_INC -I $ROOT_DIR/main"
for f in "$NANOPB_DIR"/pb_common.c "$NANOPB_DIR"/pb_decode.c "$NANOPB_DIR"/pb_encode.c; do
    obj="$BUILD_DIR/obj/nanopb/$(basename "$f" .c).o"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        "$CC" -O2 -w -c "$f" -I "$NANOPB_DIR" -o "$obj"
    fi
done
# tesla-ble objects depend on both the library version and the Mbed TLS headers they compiled against.
OBJ_KEY="$EXPECTED_TB_VER mbedtls-$MBEDTLS_REF"
if [ -f "$BUILD_DIR/obj/tb/.version" ]; then
    CACHED_OBJ_VER="$(cat "$BUILD_DIR/obj/tb/.version" 2>/dev/null || true)"
    if [ "$CACHED_OBJ_VER" != "$OBJ_KEY" ]; then
        echo "[harness] tesla-ble/Mbed TLS inputs changed ($CACHED_OBJ_VER -> $OBJ_KEY); invalidating object cache..."
        rm -rf "$BUILD_DIR/obj/tb"
        mkdir -p "$BUILD_DIR/obj/tb"
    fi
fi
echo "$OBJ_KEY" > "$BUILD_DIR/obj/tb/.version"

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
# tests/test_constants.h carries the official vehicle-command protocol test keys (never production).
"$CXX" -std=c++17 -O2 -Wall -Wextra \
    "$ROOT_DIR/test/test_tesla_ble_harness.cpp" \
    $INC -I "$TB_DIR/tests" "$BUILD_DIR/libtb.a" "$MBEDTLS_LIB" \
    -o "$BUILD_DIR/test_tesla_ble_harness"

echo "[harness] Executing integration harness..."
"$BUILD_DIR/test_tesla_ble_harness"
echo "[harness] PASS: tesla-ble integration harness verified successfully"
