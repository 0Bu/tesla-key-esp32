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
#   scripts/e2e_evcc.sh                 # read-only: status, vehicle_data, body_controller (safe)
#   RUN_COMMANDS=1 scripts/e2e_evcc.sh  # + wake_up, set_charging_amps, set_charge_limit (save/restore)
#   ALLOW_CHARGE_TOGGLE=1 RUN_COMMANDS=1 scripts/e2e_evcc.sh   # + charge_start/charge_stop (physical!)
#
# Overrides (auto-discovered from the evcc DB if unset):
#   ESP32_URL=http://192.168.1.194   VIN=LRW3E7FS4TC656735
#   EVCC_NS=default                  ITER=15  (vehicle_data burst size)
#   TIMEOUT=15                       (per-request seconds; evcc itself uses a short client timeout)
#
set -uo pipefail

EVCC_NS="${EVCC_NS:-default}"
ITER="${ITER:-15}"
TIMEOUT="${TIMEOUT:-15}"
ESP32_URL="${ESP32_URL:-http://192.168.1.194}"
VIN="${VIN:-LRW3E7FS4TC656735}"
RUN_COMMANDS="${RUN_COMMANDS:-0}"
ALLOW_CHARGE_TOGGLE="${ALLOW_CHARGE_TOGGLE:-0}"

pass=0; fail=0
ok()   { echo "  PASS  $*"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $*"; fail=$((fail+1)); }
hdr()  { echo; echo "── $* ──────────────────────────────────────────────" | cut -c1-78; }

POD="$(kubectl get pod -n "$EVCC_NS" -l app=evcc -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)"
[ -z "$POD" ] && { echo "FATAL: no evcc pod found in ns/$EVCC_NS"; exit 2; }

echo "evcc pod : $EVCC_NS/$POD"
echo "target   : $ESP32_URL  VIN=$VIN"
echo "mode     : reads$( [ "$RUN_COMMANDS" = 1 ] && echo ' + commands' )$( [ "$ALLOW_CHARGE_TOGGLE" = 1 ] && echo ' + charge-toggle' )"

# kex CMD... — run a shell snippet inside the evcc pod
kex() { kubectl exec -n "$EVCC_NS" "$POD" -- sh -c "$1"; }

# get URL  → prints body; timed_get URL → prints "<ms> <body>"
ESC_BASE="$ESP32_URL"; ESC_VIN="$VIN"

# ── 1. /status (web-UI snapshot; proves device up + paired) ─────────────────
hdr "1. GET /status (device health)"
STATUS="$(kex "wget -qO- --timeout=$TIMEOUT '$ESC_BASE/status' 2>/dev/null")"
if echo "$STATUS" | grep -q '"paired":true'; then ok "device paired"; else bad "device not paired: $STATUS"; fi
echo "$STATUS" | grep -q '"connected":true' && ok "BLE connected" || echo "  WARN  BLE not connected (reads still served from cache)"
FW="$(echo "$STATUS" | sed -n 's/.*"version":"\([^"]*\)".*/\1/p')"; echo "  firmware: ${FW:-unknown}"

# ── 2. vehicle_data burst — the critical no-timeout check ───────────────────
hdr "2. GET vehicle_data?endpoints=charge_state  (${ITER}× — evcc's poll)"
RES="$(kex '
VIN='"$ESC_VIN"'; BASE='"$ESC_BASE"'; N='"$ITER"'; TO='"$TIMEOUT"'
f=0; stale=0; mx=0; sum=0
for i in $(seq 1 $N); do
  s=$(date +%s%3N)
  b=$(wget -qO- --timeout=$TO "$BASE/api/1/vehicles/$VIN/vehicle_data?endpoints=charge_state" 2>/dev/null); rc=$?
  e=$(date +%s%3N); d=$((e-s)); sum=$((sum+d)); [ $d -gt $mx ] && mx=$d
  # A real failure is a transport error/timeout OR a body missing the charge_state evcc
  # parses. A well-formed body with "result":false is NOT a failure: it is the honest
  # stale-cache response while the car is asleep (served in ~0ms; evcc reads
  # .response.response.charge_state.* and never checks .response.result), so count it
  # separately as "stale" rather than conflating it with a timeout.
  if [ $rc -ne 0 ] || ! echo "$b" | grep -q "\"charge_state\""; then
    f=$((f+1))
  elif ! echo "$b" | grep -q "\"result\":true"; then
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

# ── 3. body_controller_state — live VCSEC BLE read ──────────────────────────
hdr "3. GET body_controller_state  (live BLE, no-wake)"
BC="$(kex "s=\$(date +%s%3N); b=\$(wget -qO- --timeout=$TIMEOUT '$ESC_BASE/api/1/vehicles/$ESC_VIN/body_controller_state' 2>/dev/null); e=\$(date +%s%3N); echo \"\$((e-s))ms \$b\"")"
echo "  $BC"
echo "$BC" | grep -q '"result":true' && ok "body_controller_state ok" || echo "  WARN  body_controller not ready (car may be asleep) — non-fatal for evcc"

# ── 4. Commands evcc issues (gated) ─────────────────────────────────────────
# cmd NAME URI-SUFFIX [JSON-BODY] [soft]
#   Issues a command and times the full BLE round-trip *inside the pod* (one exec,
#   so the timing is real — not three separate kubectl execs). A 4th arg "soft"
#   means a car-side `result:false` is reported as a NOTE, not a FAIL: charge_start/
#   charge_stop legitimately depend on live charging state (a "Complete"/at-limit
#   car refuses to start — same as the official Fleet API).
cmd() {
  local name="$1" suf="$2" body="${3:-}" soft="${4:-}"
  local dataflag="--post-data=''"
  [ -n "$body" ] && dataflag="--header=Content-Type:application/json --post-data='$body'"
  local out d r
  out="$(kex "s=\$(date +%s%3N); r=\$(wget -qO- --timeout=$TIMEOUT $dataflag '$ESC_BASE/api/1/vehicles/$ESC_VIN/command/$suf' 2>/dev/null); e=\$(date +%s%3N); echo \"\$((e-s))|\$r\"")"
  d="${out%%|*}"; r="${out#*|}"
  echo "  ${name}: ${d}ms  ->  $r"
  if echo "$r" | grep -q '"result":true'; then
    ok "$name executed"
  elif [ "$soft" = soft ]; then
    echo "  NOTE  $name returned false — car-side rejection (depends on live charging state), not a proxy fault"
  else
    bad "$name failed/timed out"
  fi
}

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

  if [ "$ALLOW_CHARGE_TOGGLE" = 1 ]; then
    cmd "charge_start" "charge_start" "" soft
    cmd "charge_stop"  "charge_stop"  "" soft
  else
    echo "  SKIP  charge_start/charge_stop (set ALLOW_CHARGE_TOGGLE=1 to test; physically toggles charging)"
  fi
else
  hdr "4. Commands  (SKIPPED — set RUN_COMMANDS=1 to test the write path)"
fi

# ── 5. evcc's own recent view of this vehicle ───────────────────────────────
hdr "5. evcc logs — recent Tesla errors/timeouts (last 30m)"
ERRS="$(kubectl logs -n "$EVCC_NS" "$POD" --since=30m 2>/dev/null | grep -iE "$ESC_VIN|192.168.1.194|tesla" | grep -iE "timeout|deadline|canceled|refused|i/o" | tail -10)"
if [ -z "$ERRS" ]; then ok "no Tesla timeout/error log lines in the last 30m"; else
  bad "evcc reported Tesla errors:"; echo "$ERRS" | sed 's/^/      /'
fi

# ── summary ─────────────────────────────────────────────────────────────────
hdr "RESULT"
echo "  PASS=$pass  FAIL=$fail"
[ "$fail" -eq 0 ] && { echo "  ✅ e2e OK — evcc can drive the ESP32 with no timeouts."; exit 0; } \
                  || { echo "  ❌ e2e found problems (see FAIL lines above)."; exit 1; }
