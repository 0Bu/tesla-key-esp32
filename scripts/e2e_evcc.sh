#!/usr/bin/env bash
#
# e2e_evcc.sh — End-to-end test of the evcc → tesla-key-esp32 → vehicle path.
#
# Runs the *exact* HTTP calls evcc's `tesla-ble` template makes, from INSIDE the
# evcc pod, so it exercises the real network path (pod → k3s node → LAN → ESP32 →
# BLE → car). The headline goal is the one that matters for evcc: every read must
# return well-formed data with NO timeouts.
#
# Usage:
#   scripts/e2e_evcc.sh                 # read-only: status, /api/proxy/1/version, vehicle_data,
#                                       #   body_controller (safe)
#   RUN_COMMANDS=1 scripts/e2e_evcc.sh  # + wake_up, set_charging_amps, set_charge_limit (save/restore),
#                                       #   door_lock/door_unlock (negative role test — requires an
#                                       #   explicit refusal; ambiguous authentication failures fail)
#   ALLOW_CHARGE_TOGGLE=1 RUN_COMMANDS=1 scripts/e2e_evcc.sh   # + charge_start/charge_stop (physical!)
#   RUN_ALL_COMMANDS=1 RUN_COMMANDS=1 scripts/e2e_evcc.sh      # + every remaining firmware command
#                                       #   (charge_port, flash_lights, honk_horn, climate, sentry,
#                                       #   scheduled_charging) — PHYSICALLY actuates the car, NOT part
#                                       #   of the evcc path; for a full post-flash command smoke test.
#
# Overrides (env vars; fall back to the built-in defaults shown below if unset):
#   ESP32_URL=http://tesla-key-esp32.local   (the device's mDNS name — resolvable from the
#                                             pod on the same LAN; override with the IP if mDNS
#                                             isn't available in your cluster)
#   VIN=<auto>                       (unset → discovered from GET $ESP32_URL/status .vin, so the
#                                     real vehicle identifier never has to live in this repo)
#   EVCC_NS=default                  ITER=15  (vehicle_data burst size)
#   TIMEOUT=15                       (per-request seconds; evcc itself uses a short client timeout)
#
set -uo pipefail

EVCC_NS="${EVCC_NS:-default}"
ITER="${ITER:-15}"
# clamp to >=1 so the pod-side vehicle_data loop never divides by zero (AVG=sum/N)
[ "$ITER" -ge 1 ] 2>/dev/null || ITER=15
TIMEOUT="${TIMEOUT:-15}"
ESP32_URL="${ESP32_URL:-http://tesla-key-esp32.local}"
VIN="${VIN:-}"   # empty → auto-discovered from the device's own /status below
RUN_COMMANDS="${RUN_COMMANDS:-0}"
ALLOW_CHARGE_TOGGLE="${ALLOW_CHARGE_TOGGLE:-0}"
RUN_ALL_COMMANDS="${RUN_ALL_COMMANDS:-0}"

pass=0; fail=0
ok()   { echo "  PASS  $*"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $*"; fail=$((fail+1)); }
hdr()  { echo; echo "── $* ──────────────────────────────────────────────" | cut -c1-78; }

# kex CMD... — stubbed or pod-executed shell snippet
kex() { kubectl exec -n "$EVCC_NS" "$POD" -- sh -c "$1"; }

cmd() {
  local name="$1" suf="$2" body="${3:-}" mode="${4:-}"
  local out d rest code r
  # Use a single nc request so that 5xx/4xx errors are not double-sent (wget -qO- discards
  # non-200 responses and caused unintentional duplicate command execution on failures).
  out="$(kex "s=\$(date +%s%3N); host=\$(echo '$ESC_BASE' | sed -e 's,^http://,,' -e 's,/.*$,,' -e 's,:.*$,,'); port=\$(echo '$ESC_BASE' | sed -n 's,^http://[^:]*:\([0-9]*\).*,\1,p'); [ -z \"\$port\" ] && port=80; raw=\$(printf 'POST /api/1/vehicles/$ESC_VIN/command/$suf HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s' \"\$host\" \"${#body}\" '$body' | nc -w $TIMEOUT \"\$host\" \"\$port\" 2>/dev/null); code=\$(echo \"\$raw\" | head -n1 | cut -d' ' -f2); b=\$(echo \"\$raw\" | sed -e '1,/^\r\{0,1\}\$/d'); e=\$(date +%s%3N); echo \"\$((e-s))|\$code|\$b\"")"
  d="${out%%|*}"; rest="${out#*|}"; code="${rest%%|*}"; r="${rest#*|}"
  echo "  ${name}: ${d}ms (HTTP ${code:-none})  ->  $r"
  local decision status msg
  decision="$(python3 -c '
import json, sys

name = sys.argv[1]
mode = sys.argv[2]
code = sys.argv[3]
raw = sys.argv[4]

if not raw or not code:
    if mode == "reject":
        print(f"FAIL|{name}: no signed reply (transport timeout) — cannot confirm role boundary")
    else:
        print(f"FAIL|{name} failed/timed out (no response)")
    sys.exit(0)

def unique_object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError("duplicate response key")
        obj[key] = value
    return obj

def reject_constant(value):
    raise ValueError("non-JSON numeric constant")

try:
    data = json.loads(raw, object_pairs_hook=unique_object, parse_constant=reject_constant)
except Exception:
    if mode == "reject":
        print(f"FAIL|{name}: invalid rejection format (malformed JSON): {raw}")
    elif mode == "soft":
        print(f"FAIL|{name}: malformed JSON response (HTTP {code}): {raw}")
    else:
        print(f"FAIL|{name} failed (malformed JSON, HTTP {code}): {raw}")
    sys.exit(0)

res_obj = None
if isinstance(data, dict):
    if "response" in data:
        if isinstance(data["response"], dict) and not any(key in data for key in ["result", "reason"]):
            res_obj = data["response"]
    else:
        res_obj = data

if not res_obj or "result" not in res_obj or not isinstance(res_obj["result"], bool):
    if mode == "reject":
        print(f"FAIL|{name}: invalid rejection format (missing boolean result): {raw}")
    elif mode == "soft":
        print(f"FAIL|{name}: invalid rejection format (HTTP {code} without result:false): {raw}")
    else:
        print(f"FAIL|{name} failed/timed out (HTTP {code}): {raw}")
    sys.exit(0)

result = res_obj["result"]

if result is True:
    if mode == "reject":
        print(f"FAIL|{name} was ACCEPTED — key has more than Charging-Manager privileges (role-boundary regression!)")
    elif code != "200":
        print(f"FAIL|{name} returned HTTP {code} with result:true: {raw}")
    else:
        print(f"PASS|{name} executed")
    sys.exit(0)

# result is False:
reason = res_obj.get("reason")
if not isinstance(reason, str) or not reason.strip():
    print(f"FAIL|{name}: invalid failure reason (expected a nonempty string): {raw}")
    sys.exit(0)
reason_l = reason.strip().lower()

# The firmware also emits this text before sending the command, for missing,
# empty or invalid SessionInfo HMACs. It cannot prove a vehicle role refusal.
if "authentication failed" in reason_l:
    print(f"FAIL|{name}: unverified authentication outcome — cannot confirm vehicle refusal: {raw}")
    sys.exit(0)

role_refusals = {"not authorized", "unauthorized", "denied"}
if mode == "reject":
    if code != "502":
        print(f"FAIL|{name}: HTTP {code} (expected 502 from vehicle rejection) — cannot confirm role boundary")
        sys.exit(0)
    if any(sub in reason_l for sub in ["not reachable", "unreachable", "timed out", "timeout", "vehicle asleep"]):
        print(f"FAIL|{name}: car unreachable (not actually refused) — cannot confirm role boundary; re-run with the car awake")
    elif reason_l in role_refusals:
        print(f"PASS|{name} correctly refused by the car (Charging-Manager role boundary holds)")
    else:
        print(f"FAIL|{name}: rejection reason not authenticated vehicle refusal: {raw}")
    sys.exit(0)

if mode == "soft":
    if code != "502":
        print(f"FAIL|{name} failed locally (HTTP {code}): {raw}")
        sys.exit(0)
    local_or_transport = [
        "not reachable",
        "unreachable",
        "timed out",
        "timeout",
        "vehicle asleep",
        "service not ready",
        "runtime unavailable",
        "runtime key is not verified",
        "out of memory",
        "invalid uri",
        "vin mismatch",
        "unknown command",
        "request body",
        "command queue full",
        "command enqueue failed",
        "command completion unavailable",
        "command deadline exhausted",
        "connection lost",
        "payload build failed",
    ]
    vehicle_responses = {
        "key not on whitelist - pairing required",
        "command rejected by vehicle",
        "vcsec command failed with error status",
        "infotainment action failed",
        "already_closed",
        "already_open",
        "already_set",
        "complete",
        "could_not_reach_service",
        "not authorized",
        "unauthorized",
        "denied",
        "not_charging",
        "charging",
        "is_charging",
        "charging_port_closed",
    }
    action_reason = reason_l
    for prefix in ["infotainment action failed: ", "action failed: "]:
        if reason_l.startswith(prefix):
            action_reason = reason_l[len(prefix):]
            break
    if any(sub in reason_l for sub in local_or_transport):
        print(f"FAIL|{name}: proxy or reachability error: {raw}")
    elif action_reason in vehicle_responses:
        print(f"NOTE|{name} returned false (HTTP {code}) — car-side rejection (depends on live state), not a proxy fault")
    else:
        print(f"FAIL|{name}: unverified failure outcome (HTTP {code}): {raw}")
    sys.exit(0)

# strict mode with result=False
print(f"FAIL|{name} failed/timed out (HTTP {code}): {raw}")
' "$name" "$mode" "${code:-}" "$r")"

  status="${decision%%|*}"
  msg="${decision#*|}"
  case "$status" in
    PASS) ok "$msg" ;;
    FAIL) bad "$msg" ;;
    NOTE) echo "  NOTE  $msg" ;;
    *) bad "$name: internal error evaluating response ($decision)" ;;
  esac
}

self_test() {
  local ESC_BASE="http://127.0.0.1" ESC_VIN="TESTVIN" TIMEOUT=5
  local mock_code="" mock_body=""

  kex() {
    echo "10|$mock_code|$mock_body"
  }

  expect_fail() {
    local test_mode="$1" test_name="$2"
    local prev_fail=$fail
    cmd "$test_name" "dummy" "{}" "$test_mode" >/dev/null 2>&1 || true
    if [ "$fail" -le "$prev_fail" ]; then
      echo "self_test FAILED: expected failure for $test_name ($test_mode), but fail count did not increase" >&2
      exit 1
    fi
  }

  expect_pass() {
    local test_mode="$1" test_name="$2"
    local prev_fail=$fail
    local prev_pass=$pass
    cmd "$test_name" "dummy" "{}" "$test_mode" >/dev/null 2>&1 || true
    if [ "$fail" -ne "$prev_fail" ]; then
      echo "self_test FAILED: expected pass for $test_name ($test_mode), but fail count increased" >&2
      exit 1
    fi
    if [ "$test_mode" != soft ] && [ "$pass" -ne $((prev_pass + 1)) ]; then
      echo "self_test FAILED: expected a counted pass for $test_name ($test_mode)" >&2
      exit 1
    fi
  }

  # Soft mode tests:
  # 1. HTTP 400, invalid JSON -> must fail
  mock_code="400"; mock_body='{"error":"invalid JSON"}'
  expect_fail "soft" "soft HTTP 400 invalid JSON"

  # 2. HTTP 503, out of memory -> must fail
  mock_code="503"; mock_body='{"error":"out of memory"}'
  expect_fail "soft" "soft HTTP 503 out of memory"

  # 3. HTTP 502, vehicle service not ready -> must fail
  mock_code="502"; mock_body='{"result":false,"reason":"vehicle service not ready"}'
  expect_fail "soft" "soft HTTP 502 vehicle service not ready"

  # 4. HTTP 502, locally generated vehicle asleep -> must fail
  mock_code="502"; mock_body='{"result":false,"reason":"vehicle asleep"}'
  expect_fail "soft" "soft HTTP 502 vehicle asleep"

  # 5. Soft mode with harmless vehicle-side rejection -> passes (softened)
  mock_code="502"; mock_body='{"result":false,"reason":"already_closed"}'
  expect_pass "soft" "soft HTTP 502 harmless vehicle rejection"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"complete"}}'
  expect_pass "soft" "soft HTTP 502 vehicle charging complete"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"command rejected by vehicle"}}'
  expect_pass "soft" "soft HTTP 502 command rejected by vehicle"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"action failed: charging_port_closed"}}'
  expect_pass "soft" "soft HTTP 502 action failed: charging_port_closed"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"key not on whitelist - pairing required"}}'
  expect_pass "soft" "soft HTTP 502 key not on whitelist"

  # Actual local producer reasons from vehicle_commands.cpp / http_api.cpp -> must fail!
  mock_code="502"; mock_body='{"response":{"result":false,"reason":"command queue full"}}'
  expect_fail "soft" "soft HTTP 502 command queue full"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"command enqueue failed"}}'
  expect_fail "soft" "soft HTTP 502 command enqueue failed"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"command completion unavailable"}}'
  expect_fail "soft" "soft HTTP 502 command completion unavailable"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"command deadline exhausted"}}'
  expect_fail "soft" "soft HTTP 502 command deadline exhausted"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"command deadline exhausted waiting for another request"}}'
  expect_fail "soft" "soft HTTP 502 command deadline exhausted waiting for request"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"runtime key is not verified; reboot or regenerate required"}}'
  expect_fail "soft" "soft HTTP 502 runtime key not verified"

  # Malformed JSON in soft, reject, and strict modes -> must fail!
  mock_code="502"; mock_body='{"response":{"result":false'
  expect_fail "soft" "soft HTTP 502 truncated JSON"

  mock_code="502"; mock_body='{"response":{"result":false'
  expect_fail "reject" "reject HTTP 502 truncated JSON"

  mock_code="200"; mock_body='{"response":{"result":true'
  expect_fail "" "strict HTTP 200 truncated JSON"

  # Unknown failure outcome in soft mode -> must fail!
  mock_code="502"; mock_body='{"result":false,"reason":"mystery failure"}'
  expect_fail "soft" "soft HTTP 502 unknown failure reason"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":null}}'
  expect_fail "soft" "soft HTTP 502 null reason"

  # Authentication errors are also emitted locally before a command payload.
  mock_code="502"; mock_body='{"result":false,"reason":"authentication failed"}'
  expect_fail "reject" "reject HTTP 502 ambiguous authentication failure"
  expect_fail "soft" "soft HTTP 502 ambiguous authentication failure"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"signed message authentication failed"}}'
  expect_fail "reject" "reject HTTP 502 signed message authentication failure"
  expect_fail "soft" "soft HTTP 502 signed message authentication failure"

  mock_code="502"; mock_body='{"response":{"result":false,"reason":"VCSEC authentication failed"}}'
  expect_fail "reject" "reject HTTP 502 VCSEC authentication failure"
  expect_fail "soft" "soft HTTP 502 VCSEC authentication failure"

  # Schema-invalid and mixed-scope reasons must never certify a vehicle response.
  for mock_body in \
    '{"response":{"result":false,"reason":{"authentication failed":true}}}' \
    '{"response":{"result":false,"reason":["denied"]}}' \
    '{"response":{"result":false,"reason":null},"reason":"denied"}' \
    '{"response":{"result":false,"reason":"denied"},"result":true}' \
    '{"response":null,"result":false,"reason":"denied"}' \
    '{"result":false,"reason":"denied","reason":"not authorized"}'; do
    expect_fail "reject" "reject invalid reason schema/scope"
    expect_fail "soft" "soft invalid reason schema/scope"
  done

  mock_body='{"result":false,"reason":"authorization denied response was not authenticated"}'
  expect_fail "reject" "reject unverified refusal substring"
  expect_fail "soft" "soft unverified refusal substring"

  mock_body='{"result":false,"reason":"local verification failed after charging request"}'
  expect_fail "soft" "soft local error containing charging"

  mock_body='{"response":{"result":false,"reason":"Infotainment action failed"}}'
  expect_pass "soft" "soft explicit CarServer error without optional reason"
  expect_fail "reject" "reject CarServer error without a role reason"

  for constant in NaN Infinity -Infinity; do
    mock_body="{\"result\":false,\"reason\":\"denied\",\"extra\":$constant}"
    expect_fail "reject" "reject non-JSON numeric constant"
    expect_fail "soft" "soft non-JSON numeric constant"
    mock_code="200"; mock_body="{\"result\":true,\"extra\":$constant}"
    expect_fail "" "strict non-JSON numeric constant"
    mock_code="502"
  done

  # Reject mode needs an explicit refusal rather than a substring match.

  mock_code="502"; mock_body='{"result":false,"reason":"not authorized"}'
  expect_pass "reject" "reject HTTP 502 not authorized"

  mock_code="502"; mock_body='{"result":false,"reason":"unauthorized"}'
  expect_pass "reject" "reject HTTP 502 unauthorized"

  mock_code="502"; mock_body='{"result":false,"reason":"denied"}'
  expect_pass "reject" "reject HTTP 502 denied"

  # 7. Reject mode with local 400, 503, or reachability timeout -> must fail!
  mock_code="400"; mock_body='{"error":"bad request"}'
  expect_fail "reject" "reject HTTP 400"

  mock_code="503"; mock_body='{"error":"out of memory"}'
  expect_fail "reject" "reject HTTP 503"

  mock_code="502"; mock_body='{"result":false,"reason":"not reachable"}'
  expect_fail "reject" "reject HTTP 502 not reachable"

  mock_code="502"; mock_body='{"result":false,"reason":"vehicle asleep"}'
  expect_fail "reject" "reject HTTP 502 vehicle asleep"

  # 8. Success execution (HTTP 200 with result:true) in default mode -> passes
  mock_code="200"; mock_body='{"result":true}'
  expect_pass "" "normal HTTP 200 result:true"

  mock_code="200"; mock_body='{"response":{"result":true,"command":"wake_up","vin":"TESTVIN"}}'
  expect_pass "" "normal HTTP 200 nested response result:true"

  mock_body='{"response":{"result":false,"result":true}}'
  expect_fail "" "strict HTTP 200 duplicate result"

  # Non-200 status with result:true must fail in strict mode
  mock_code="502"; mock_body='{"result":true}'
  expect_fail "" "strict HTTP 502 result:true"

  mock_code="500"; mock_body='{"result":true}'
  expect_fail "" "strict HTTP 500 result:true"

  echo "e2e_evcc self-test: PASS"
  return 0
}

if [ "${1:-}" = "--self-test" ]; then
  self_test
  exit 0
fi

POD="$(kubectl get pod -n "$EVCC_NS" -l app=evcc -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)"
[ -z "$POD" ] && { echo "FATAL: no evcc pod found in ns/$EVCC_NS"; exit 2; }


# Discover the VIN from the device itself unless the caller pinned one. The firmware is the
# single source of truth (GET /status → "vin"), so no real vehicle identifier has to be
# committed to this repo — the test learns it from whichever board ESP32_URL points at.
if [ -z "$VIN" ]; then
  VIN="$(kex "wget -qO- --timeout=$TIMEOUT '$ESP32_URL/status' 2>/dev/null" | sed -n 's/.*"vin":"\([^"]*\)".*/\1/p')"
  [ -n "$VIN" ] && echo "VIN      : discovered $VIN from $ESP32_URL/status" \
                || { echo "FATAL: could not read VIN from $ESP32_URL/status — set VIN=… or check the device is reachable from the pod"; exit 2; }
fi

echo "evcc pod : $EVCC_NS/$POD"
echo "target   : $ESP32_URL  VIN=$VIN"
echo "mode     : reads$( [ "$RUN_COMMANDS" = 1 ] && echo ' + commands' )$( [ "$ALLOW_CHARGE_TOGGLE" = 1 ] && echo ' + charge-toggle' )$( [ "$RUN_ALL_COMMANDS" = 1 ] && echo ' + all-commands' )"

# get URL  → prints body; timed_get URL → prints "<ms> <body>"
ESC_BASE="$ESP32_URL"; ESC_VIN="$VIN"

# ── 1. /status + /api/proxy/1/version (device health + evcc proxy detect) ───
hdr "1. GET /status + /api/proxy/1/version (device + proxy health)"
STATUS="$(kex "wget -qO- --timeout=$TIMEOUT '$ESC_BASE/status' 2>/dev/null")"
if echo "$STATUS" | grep -q '"paired":true'; then ok "device paired"; else bad "device not paired: $STATUS"; fi
echo "$STATUS" | grep -q '"connected":true' && ok "BLE connected" || echo "  WARN  BLE not connected (reads still served from cache)"
FW="$(echo "$STATUS" | sed -n 's/.*"version":"\([^"]*\)".*/\1/p')"; echo "  firmware: ${FW:-unknown}"
# read-only telemetry surface (feeds the HA/MQTT bridge); present once the rotating bg poll has run
echo "$STATUS" | grep -q '"tele"' && ok "telemetry block present (tele)" \
  || echo "  WARN  no tele block yet (rotating background poll may not have completed a cycle)"

# /api/proxy/1/version — proxy API-surface endpoint. The firmware's own web UI + OTA read it;
# evcc itself does NOT call it (the evcc tesla-ble template only POSTs commands + GETs
# vehicle_data). Checked here for firmware completeness + version coherence with /status.
VER="$(kex "wget -qO- --timeout=$TIMEOUT '$ESC_BASE/api/proxy/1/version' 2>/dev/null")"
echo "  proxy: $VER"
if echo "$VER" | grep -q '"platform"' && echo "$VER" | grep -q '"version"'; then
  ok "proxy version endpoint ok (/api/proxy/1/version)"
else
  bad "proxy version endpoint missing/malformed: $VER"
fi
# coherence: the proxy reports "<status.version>-esp32"; a mismatch means /status and the OTA
# image disagree (stale build / half-applied OTA).
if [ -n "$FW" ]; then
  echo "$VER" | grep -q "\"${FW}-esp32\"" && ok "version coherent (${FW}-esp32)" \
    || echo "  WARN  version mismatch: /status=$FW  vs  /api/proxy/1/version=$VER"
fi

# ── 2. vehicle_data burst — the critical no-timeout check ───────────────────
hdr "2. GET vehicle_data?endpoints=charge_state  (${ITER}× — evcc's poll)"
RES="$(kex '
VIN='"$ESC_VIN"'; BASE='"$ESC_BASE"'; N='"$ITER"'; TO='"$TIMEOUT"'
host=$(echo "$BASE" | sed -e "s,^http://,," -e "s,/.*$,," -e "s,:.*$,,"); port=$(echo "$BASE" | sed -n "s,^http://[^:]*:\([0-9]*\).*,\1,p"); [ -z "$port" ] && port=80
f=0; stale=0; mx=0; sum=0
for i in $(seq 1 $N); do
  s=$(date +%s%3N)
  raw=$(printf "GET /api/1/vehicles/%s/vehicle_data?endpoints=charge_state HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" "$VIN" "$host" | nc -w $TO "$host" "$port" 2>/dev/null)
  code=$(echo "$raw" | head -n1 | cut -d" " -f2)
  b=$(echo "$raw" | sed -e "1,/^\r\{0,1\}$/d")
  e=$(date +%s%3N); d=$((e-s)); sum=$((sum+d)); [ $d -gt $mx ] && mx=$d
  # A real failure is a transport error/timeout OR an unexpected HTTP status code
  # (neither 200 nor 503) OR a body missing the charge_state evcc parses.
  # A well-formed body with HTTP 503 or "result":false is the honest stale-cache
  # response while the car is asleep (served in ~0ms; evcc reads
  # .response.response.charge_state.* and never checks .response.result), so count it
  # separately as "stale" rather than conflating it with a timeout.
  if [ -z "$b" ] || [ -z "$code" ] || { [ "$code" != "200" ] && [ "$code" != "503" ]; } || ! echo "$b" | grep -q "\"charge_state\""; then
    f=$((f+1))
  elif [ "$code" = "503" ] || ! echo "$b" | grep -q "\"result\":true"; then
    stale=$((stale+1))
  fi
done
echo "FAILS=$f STALE=$stale MAX=$mx AVG=$((sum/N))"
echo "SAMPLE=$b"
')"
FAILS=$(echo "$RES" | sed -n 's/.*FAILS=\([0-9]*\).*/\1/p')
STALE=$(echo "$RES" | sed -n 's/.*STALE=\([0-9]*\).*/\1/p')
MAXMS=$(echo "$RES" | sed -n 's/.*MAX=\([0-9]*\).*/\1/p')
AVGMS=$(echo "$RES" | sed -n 's/.*AVG=\([0-9]*\).*/\1/p')
SAMPLE=$(echo "$RES" | sed -n 's/^SAMPLE=//p')
echo "  latency: avg=${AVGMS}ms max=${MAXMS}ms   transport failures: ${FAILS}/${ITER}   stale (car asleep): ${STALE:-0}/${ITER}"
[ "${FAILS:-1}" = 0 ] && ok "0 timeouts/transport failures over ${ITER} polls" || bad "${FAILS} timeout(s)/transport failure(s)"
[ "${STALE:-0}" != 0 ] && echo "  NOTE  ${STALE}/${ITER} returned result:false (stale cache — car asleep); well-formed & ~0ms, evcc-safe (it reads charge_state, not result)"
# evcc parses these fields; all must be present and numeric (battery_range as float)
for fld in charging_state battery_level charge_limit_soc charger_power charge_rate charge_amps battery_range; do
  echo "$SAMPLE" | grep -q "\"$fld\"" && ok "field present: $fld" || bad "field MISSING: $fld"
done

# ── 3. body_controller_state — live BLE read ──────────────────────────
hdr "3. GET body_controller_state  (live BLE, no-wake)"
BC="$(kex "s=\$(date +%s%3N); b=\$(wget -qO- --timeout=$TIMEOUT '$ESC_BASE/api/1/vehicles/$ESC_VIN/body_controller_state' 2>/dev/null); e=\$(date +%s%3N); echo \"\$((e-s))ms \$b\"")"
echo "  $BC"
echo "$BC" | grep -q '"result":true' && ok "body_controller_state ok" || echo "  WARN  body_controller not ready (car may be asleep) — non-fatal for evcc"

# ── 4. Commands evcc issues (gated) ─────────────────────────────────────────
# cmd NAME URI-SUFFIX [JSON-BODY] [MODE]
#   Issues a command and times the full BLE round-trip *inside the pod* (one exec,
#   so the timing is real — not three separate kubectl execs). MODE (4th arg):
#     ""       — strict: car-side `result:false` is a FAIL.
#     "soft"   — `result:false` is a NOTE, not a FAIL: charge_start/charge_stop (and the
#                extended sweep) legitimately depend on live state (a "Complete"/at-limit
#                car refuses to start — same as the official Fleet API).
#     "reject" — inverted: the command MUST be refused. NOTE the firmware does not
#                always return 200: it returns HTTP 502 with result:false on command rejection
#                or reachability error (reason="vehicle not reachable"), and HTTP 503 on
#                unavailable charge state. Therefore, result:false alone does NOT prove a
#                successful refusal. PASS only on a car-side refusal (result:false with a
#                non-reachability reason); result:true is a security regression (FAIL);
#                a reachability/timeout reason is FAIL "can't confirm — re-run awake".
#                This is what stops a sleeping car from false-PASSing.


if [ "$RUN_COMMANDS" = 1 ]; then
  hdr "4. Commands (write path → signed BLE → car)"

  # capture current amps + limit so we can restore
  CUR_AMPS=$(echo "$SAMPLE"  | sed -n 's/.*"charge_amps":\([0-9]*\).*/\1/p')
  CUR_LIM=$(echo "$SAMPLE"   | sed -n 's/.*"charge_limit_soc":\([0-9]*\).*/\1/p')
  echo "  baseline: charge_amps=${CUR_AMPS:-?}  charge_limit_soc=${CUR_LIM:-?}"

  cmd "wake_up" "wake_up"

  # set_charging_amps: set to current value (no real change), proves the BLE write path
  cmd "set_charging_amps(${CUR_AMPS:-6})" "set_charging_amps" "{\"charging_amps\":${CUR_AMPS:-6}}"

  # set_charge_limit: a real change (CUR-10) then restore to CUR. Re-asserting the
  # *same* value is a no-op the car rejects when Complete, so we change-and-restore.
  if [ -n "${CUR_LIM:-}" ] && [ "$CUR_LIM" -ge 60 ]; then
    NEWLIM=$((CUR_LIM-10))
    cmd "set_charge_limit(${NEWLIM})" "set_charge_limit" "{\"percent\":${NEWLIM}}"
    cmd "set_charge_limit(${CUR_LIM}) [restore]" "set_charge_limit" "{\"percent\":${CUR_LIM}}"
  fi

  # Role-boundary negative test: the key is enrolled Charging-Manager-only, so the car
  # MUST refuse lock/unlock (see docs — door_lock/unlock exist for API completeness only).
  # A success here would mean the key carries owner privileges → security regression.
  cmd "door_lock"   "door_lock"   "" reject
  cmd "door_unlock" "door_unlock" "" reject

  if [ "$ALLOW_CHARGE_TOGGLE" = 1 ]; then
    cmd "charge_start" "charge_start" "" soft
    cmd "charge_stop"  "charge_stop"  "" soft
  else
    echo "  SKIP  charge_start/charge_stop (set ALLOW_CHARGE_TOGGLE=1 to test; physically toggles charging)"
  fi

  # Extended sweep: exercise every remaining firmware command so a post-flash smoke test
  # proves the BLE write path + tesla-ble builder works for the whole command surface (not
  # just the evcc subset). These PHYSICALLY actuate the car; all are "soft" because car-side
  # acceptance depends on live state — the point is that each dispatches and round-trips
  # without faulting the proxy. Paired commands are issued both ways to net out the state.
  if [ "$RUN_ALL_COMMANDS" = 1 ]; then
    hdr "4b. Extended command sweep (PHYSICAL — every remaining firmware command)"
    cmd "charge_port_door_open"        "charge_port_door_open"        ""                                   soft
    cmd "charge_port_door_close"       "charge_port_door_close"       ""                                   soft
    cmd "flash_lights"                 "flash_lights"                 ""                                   soft
    cmd "honk_horn"                    "honk_horn"                    ""                                   soft
    cmd "auto_conditioning_start"      "auto_conditioning_start"      ""                                   soft
    cmd "auto_conditioning_stop"       "auto_conditioning_stop"       ""                                   soft
    cmd "set_sentry_mode(on)"          "set_sentry_mode"              '{"on":true}'                        soft
    cmd "set_sentry_mode(off)"         "set_sentry_mode"              '{"on":false}'                       soft
    cmd "set_scheduled_charging(on@120)" "set_scheduled_charging"     '{"enable":true,"start_minutes":120}' soft
    cmd "set_scheduled_charging(off)"  "set_scheduled_charging"       '{"enable":false,"start_minutes":0}'  soft
  else
    echo "  SKIP  extended sweep (set RUN_ALL_COMMANDS=1 to exercise every firmware command — PHYSICAL)"
  fi
else
  hdr "4. Commands  (SKIPPED — set RUN_COMMANDS=1 to test the write path)"
fi

# ── 5. evcc's own recent view of this vehicle ───────────────────────────────
hdr "5. evcc logs — recent Tesla errors/timeouts (last 30m)"
# match on the VIN, the device host (from ESP32_URL, minus scheme/port/path), or "tesla"
ESP32_HOST="${ESP32_URL#*://}"; ESP32_HOST="${ESP32_HOST%%[:/]*}"
ERRS="$(kubectl logs -n "$EVCC_NS" "$POD" --since=30m 2>/dev/null | grep -iE "$ESC_VIN|$ESP32_HOST|tesla" | grep -iE "timeout|deadline|canceled|refused|i/o" | tail -10)"
if [ -z "$ERRS" ]; then ok "no Tesla timeout/error log lines in the last 30m"; else
  bad "evcc reported Tesla errors:"; echo "$ERRS" | sed 's/^/      /'
fi

# ── summary ─────────────────────────────────────────────────────────────────
hdr "RESULT"
echo "  PASS=$pass  FAIL=$fail"
[ "$fail" -eq 0 ] && { echo "  ✅ e2e OK — evcc can drive the ESP32 with no timeouts."; exit 0; } \
                  || { echo "  ❌ e2e found problems (see FAIL lines above)."; exit 1; }
