#!/usr/bin/env bash
# Host-side integration harness test against the real yoziru/tesla-ble v6.2.0 library (with the
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

mkdir -p "$BUILD_DIR/obj" "$CACHE_DIR"

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

# Exact repository-owned sources are admitted before any compiler or dependency configure effect.
# Keep the existing Mbed TLS source/helper policy separate from Tesla/nanopb admission.
MBEDTLS_REF="a2b32072ea898afc1ed5b6caf6931e36028c91d6"
CC="${CC:-clang}"
CXX="${CXX:-clang++}"
if ! command -v "$CC" >/dev/null 2>&1; then CC=gcc; fi
if ! command -v "$CXX" >/dev/null 2>&1; then CXX=g++; fi

admit_or_fail() {
    local proof status
    if proof="$(python3 "$ROOT_DIR/scripts/dependency_source.py" "$@")"; then
        printf '%s\n' "$proof"
        return 0
    else
        status=$?
    fi
    if [ "$status" = 75 ] && [ "$REQUIRE_ALL" = 0 ] && [ "${CI:-}" != true ]; then
        return 75
    fi
    echo "[harness] ERROR: exact dependency source admission failed" >&2
    return 1
}
# Existing mismatches are always hard failures, even when a different cold fetch is offline.
python3 "$ROOT_DIR/scripts/dependency_source.py" harness-preflight \
    --root "$ROOT_DIR" --source "$CACHE_DIR" >/dev/null

if NANOPB_PROOF="$(admit_or_fail nanopb --root "$ROOT_DIR" \
        --source "$CACHE_DIR/nanopb-pristine" --object-root "$BUILD_DIR/obj" \
        --cache-salt "$CC mbedtls-$MBEDTLS_REF")"; then
    :
else
    status=$?
    if [ "$status" = 75 ]; then
        echo "[harness] cold source transport unavailable — skipping optional harness"
        exit 0
    fi
    exit 1
fi
if TB_PROOF="$(admit_or_fail tesla-ble --root "$ROOT_DIR" \
        --source "$CACHE_DIR/tesla-pristine" --workspace "$CACHE_DIR/tesla-patched" \
        --object-root "$BUILD_DIR/obj" \
        --cache-salt "$CC $CXX mbedtls-$MBEDTLS_REF loglevel-2")"; then
    :
else
    status=$?
    if [ "$status" = 75 ]; then
        echo "[harness] cold source transport unavailable — skipping optional harness"
        exit 0
    fi
    exit 1
fi
NANOPB_DIR="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["source"])' "$NANOPB_PROOF")"
NANOPB_OBJ_DIR="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["objectDirectory"])' "$NANOPB_PROOF")"
TB_DIR="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["source"])' "$TB_PROOF")"
TB_OBJ_DIR="$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["objectDirectory"])' "$TB_PROOF")"

MBEDTLS_DIR="$CACHE_DIR/mbedtls-$MBEDTLS_REF"
fetch_commit_or_fail "espressif/mbedtls" "$MBEDTLS_REF" "https://github.com/espressif/mbedtls.git" "$MBEDTLS_DIR"

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
    obj="$NANOPB_OBJ_DIR/$(basename "$f" .c).o"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        "$CC" -O2 -w -c "$f" -I "$NANOPB_DIR" -o "$obj"
    fi
done
# Both object namespaces already include the complete admitted bytes/modes, record/patch
# identity, compiler selectors and unchanged crypto/log policy; unchanged mtimes cannot select
# an old-source namespace. Existing foreign or aliased cache paths are never invalidated.
TB_LOG_FLAGS="-DTESLA_BLE_LOG_LEVEL=2"

echo "[harness] Compiling tesla-ble protobuf descriptors..."
while IFS= read -r -d '' f; do
    rel="${f#"$TB_DIR/generated/src/"}"
    flat_name="${rel//\//_}"
    flat_name="${flat_name%.c}.o"
    obj="$TB_OBJ_DIR/$flat_name"
    if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
        "$CC" -O2 -w -c "$f" $INC -o "$obj"
    fi
done < <(find "$TB_DIR/generated/src" -name '*.pb.c' -print0)

echo "[harness] Compiling tesla-ble core sources..."
for f in "$TB_DIR"/src/*.cpp; do
    if [ -f "$f" ] && [ "$(basename "$f")" != "vehicle.cpp" ]; then
        obj="$TB_OBJ_DIR/$(basename "$f" .cpp).o"
        if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ]; then
            "$CXX" -std=c++20 -O2 -w "$TB_LOG_FLAGS" -c "$f" $INC -o "$obj"
        fi
    fi
done

ar rcs "$BUILD_DIR/libtb.a" "$TB_OBJ_DIR"/*.o "$NANOPB_OBJ_DIR"/*.o

echo "[harness] Compiling test_tesla_ble_harness..."
# Compile the actual firmware response adapter against the real protocol/crypto types, replacing
# only its IDF facade and telemetry sinks. Never copy its admission decisions into the harness.
python3 - "$ROOT_DIR/main/vehicle_telemetry.cpp" "$BUILD_DIR/carserver-handler.inc" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
marker = "void VehicleController::handle_carserver_frame_("
if source.count(marker) != 1:
    raise SystemExit("expected exactly one native CarServer response adapter")
start = source.index(marker)
end = source.index("\n}", start) + 2
Path(sys.argv[2]).write_text(source[start:end] + "\n")
PY
# Compile the native VCSEC builders, routing, authentication and response entry point unchanged.
python3 - "$ROOT_DIR/main/vehicle_telemetry.cpp" "$BUILD_DIR/vcsec-native.inc" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
begin, end = "// VCSEC_ROUTING_SCOPE_BEGIN", "// VCSEC_ROUTING_SCOPE_END"
if source.count(begin) != 1 or source.count(end) != 1:
    raise SystemExit("expected exactly one native routing-address scope")
methods = [source.split(begin, 1)[1].split(end, 1)[0]]
for result, name in (
    ("void", "retire_vcsec_contexts_"), ("bool", "initialize_routing_address_"),
    ("int", "build_vcsec_attempt_"), ("bool", "vcsec_response_matches_"),
    ("bool", "deliver_vcsec_response_"), ("void", "handle_empty_vcsec_reply_"),
    ("bool", "response_matches_current_"), ("bool", "authenticate_fault_reply_"),
    ("void", "handle_vcsec_frame_"), ("void", "handle_signed_message_fault_"),
    ("void", "handle_session_info_frame_"), ("void", "process_rx_frame_"),
    ("void", "drive_command_runner_"), ("bool", "apply_ble_link_state_"),
    ("tk::SessionRuntimeState", "runtime_session_observation_locked_"),
    ("void", "publish_session_runtime_locked_"), ("void", "invalidate_session_runtime_locked_"),
    ("tk::SessionAvailability", "session_availability"), ("bool", "session_available"),
    ("bool", "persist_session_"), ("bool", "save_session_runtime_locked_"), ("bool", "load_nvs_sessions_"),
    ("void", "establish_verified_session_locked_"),
):
    marker = result + " VehicleController::" + name + "("
    if source.count(marker) != 1:
        raise SystemExit("expected exactly one native VCSEC method: " + name)
    start = source.index(marker)
    finish = source.index("\n}", start) + 2
    methods.append(source[start:finish])
Path(sys.argv[2]).write_text("\n\n".join(methods) + "\n")
PY
# The identity-cleanup regression compiles the actual native methods. Replace only the
# controller class name; storage, locking, link and cache effects are supplied by host fixtures.
python3 - "$ROOT_DIR/main/vehicle_pairing.cpp" "$BUILD_DIR/pairing-cleanup.inc" "$ROOT_DIR/main/vehicle_telemetry.cpp" <<'PY'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
methods = []
for name in ("clear_session_and_cache_", "finish_key_rotation_cleanup_"):
    marker = "bool VehicleController::" + name + "("
    if source.count(marker) != 1:
        raise SystemExit("expected exactly one native pairing cleanup method: " + name)
    start = source.index(marker)
    end = source.index("\n}", start) + 2
    methods.append(source[start:end].replace("VehicleController::", "PairingCleanupController::", 1))
source = Path(sys.argv[3]).read_text()
marker = "void VehicleController::retire_vcsec_contexts_("
if source.count(marker) != 1:
    raise SystemExit("expected exactly one native VCSEC retirement method")
start = source.index(marker)
end = source.index("\n}", start) + 2
methods.append(source[start:end].replace("VehicleController::", "PairingCleanupController::", 1))
Path(sys.argv[2]).write_text("\n\n".join(methods) + "\n")
PY
# Exercise unchanged native result completion with deterministic semaphore/clock sinks.
python3 - "$ROOT_DIR/main/vehicle_commands.cpp" "$ROOT_DIR/main/vehicle_ctrl.hpp" "$BUILD_DIR" <<'PY_COMPLETION'
from pathlib import Path
import sys
source, header = Path(sys.argv[1]).read_text(), Path(sys.argv[2]).read_text()
out = Path(sys.argv[3])
types = []
for name in ("CommandCompletion", "CommandOutcome"):
    start = header.index("    struct " + name + " {")
    end = header.index("\n    };", start) + len("\n    };")
    types.append(header[start:end])
(out / "completion-types.inc").write_text("\n".join(types) + "\n")
methods = []
for result, name in (("void", "publish_command_outcome_"), ("void", "invalidate_and_flush_"),
                     ("VehicleController::CommandOutcome", "await_completion_"),
                     ("VehicleController::ResultCb", "make_result_cb_")):
    marker = result + " VehicleController::" + name + "("
    if source.count(marker) != 1:
        raise SystemExit("expected one native completion method: " + name)
    start = source.index(marker)
    end = source.index("\n}", start) + 2
    methods.append(source[start:end].replace("VehicleController::", "CompletionController::"))
(out / "completion-native.inc").write_text("\n\n".join(methods) + "\n")
marker = "        auto on_done = [cb]("
parts = source.split(marker)[1:]
adapters = ["auto on_done = [cb](" + part.split("\n        };", 1)[0] + "\n};" for part in parts]
if len(adapters) != 2 or adapters[0] != adapters[1]:
    raise SystemExit("VCSEC and Infotainment must preserve the same typed completion adapter")
(out / "completion-on-done.inc").write_text(adapters[0] + "\n")
methods = []
for result, name in (("bool", "send_vcsec_"), ("VehicleController::CommandOutcome", "send_vcsec_outcome_"),
                     ("bool", "wake_up")):
    marker = result + " VehicleController::" + name + "("
    if source.count(marker) != 1:
        raise SystemExit("expected one native wake method: " + name)
    start = source.index(marker)
    end = source.index("\n}", start) + 2
    methods.append(source[start:end].replace("VehicleController::", "WakeController::"))
(out / "wake-native.inc").write_text("\n\n".join(methods) + "\n")
clocks = []
for result, name in (("uint32_t", "deadline_in_"), ("TickType_t", "ticks_until_"),
                     ("int", "remaining_ms_"), ("uint32_t", "capped_deadline_")):
    marker = "    static " + result + " " + name + "("
    if header.count(marker) != 1:
        raise SystemExit("expected one native clock method: " + name)
    start = header.index(marker)
    end = header.index("\n    }", start) + len("\n    }")
    clocks.append(header[start:end])
(out / "wake-clock.inc").write_text("\n".join(clocks) + "\n")
PY_COMPLETION
# Compile complete actual producer/mailbox/consumer methods, including the real RxFramer path.
# A failed generation must stop before the test compiler can consume any old generated include.
python3 "$ROOT_DIR/test/generate_ble_deferred_adapter.py" "$ROOT_DIR" "$BUILD_DIR"
# tests/test_constants.h carries the official vehicle-command protocol test keys (never production).
"$CXX" -std=c++20 -O2 -Wall -Wextra "$TB_LOG_FLAGS" \
    "$ROOT_DIR/test/test_tesla_ble_harness.cpp" \
    $INC -I "$TB_DIR/tests" -I "$BUILD_DIR" -I "$ROOT_DIR/test/stubs" "$BUILD_DIR/libtb.a" "$MBEDTLS_LIB" \
    -o "$BUILD_DIR/test_tesla_ble_harness"

echo "[harness] Executing integration harness..."
"$BUILD_DIR/test_tesla_ble_harness"
echo "[harness] Executing dependency failure/logging guards..."
CXX="$CXX" python3 "$ROOT_DIR/test/test_dependency_guards.py" --component "$TB_DIR"
if [ "$(uname -s)" = Linux ]; then
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 LSAN_OPTIONS=exitcode=97 CXX="$CXX" \
        python3 "$ROOT_DIR/test/test_dependency_guards.py" --component "$TB_DIR" --sanitize
fi
echo "[harness] PASS: tesla-ble integration harness verified successfully"
