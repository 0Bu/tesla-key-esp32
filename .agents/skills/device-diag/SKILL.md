---
name: device-diag
description: Read-only, cache-only triage of tesla-key-esp32 health from existing evidence or, only with explicit user approval for the live board/target, passive GET /status, /diag, and /heap reads. Never wake or command the vehicle, change diagnostic state, pair, flash, or OTA; diagnose and hand off only.
---

# Device diagnosis

Invoke `$device-diag`. Read [AGENTS.md](../../../AGENTS.md). Prefer existing logs/status/captures.
This skill is read-only and cache-only: diagnose and hand off, without wake, commands, pairing,
configuration, signing, flash/OTA, explicit NVS operations or crash/log deletion.

An approved `GET /status` can make the firmware persist a missing or invalid `paired_at` date
when a session exists and the clock is authoritative. This is autonomous metadata stamping, not
an explicit configuration or identity operation. Prefer collected evidence when even that storage
side effect must be avoided; do not describe a live status read as guaranteeing unchanged NVS.

Before live HTTP, require explicit user approval for the identified board/host and read endpoints.
A generic diagnosis request grants none. Default requests do not change `verbose`/`clear`; those
parameters, a serial session that can toggle reset, and any recovery need separate scope.

[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md) owns pairing lifecycle and invalidation.
[API](../../../docs/API.md#management) owns status fields/units/presence;
[Architecture](../../../docs/ARCHITECTURE.md#sleep--link-state-the-single-source-of-truth) owns link proof.
The host may be `tesla-key-esp32.local` or the explicitly approved board IP.

## 1. Status

Use collected JSON first. If the exact GET is approved, inspect these vitals:

```bash
curl -s http://<host>/status | jq '{
  version, paired, reauth, link, vcsec_sleep,
  wifi: {ssid: .wifi.ssid, rssi: .wifi.rssi},
  eth: .eth,
  ble:  {connected: .ble.connected, rssi: .ble.rssi,
         connect_fail: .ble.connect_fail, car_connectable: .ble.car_connectable},
  mqtt: {configured: .mqtt.configured, connected: .mqtt.connected,
         tls: .mqtt.tls, error: .mqtt.error},
  sys:  {board_mac: .sys.board_mac,
         largest_block: .sys.largest_block, free_heap: .sys.free_heap,
         uptime_s: .sys.uptime_s, reset_reason: .sys.reset_reason,
         safe_mode: .sys.safe_mode,
         stack_min_free_bytes: .sys.stack_min_free_bytes},
  last_seen_s, last_reboot, last_crash
}'
```

Read INTERNAL largest block, reset reason, safe mode, pairing/reauth, link, transport and integration
errors before proposing a cause. Stack minima are per-boot historical bytes; missing means unsampled,
zero is real. Board MAC identifies hardware and remains visible in redacted exports.

If approved, `/api/proxy/1/version` confirms identity: its fixed `-esp32` version suffix applies on
all four chips; separate platform names the actual target. Strip the suffix when comparing to status.
`vehicle_data` reads cache only; active stale ChargeState returns 503, idle cache may legitimately age.

Link `unknown` means no evidence since startup; `unreachable` means no recent signed contact.
`idle` is normal parked reachability without proven sleep; `asleep` needs debounced VCSEC evidence.
VCSEC AWAKE alone does not prove live `awake`. Do not treat ordinary sleep as a fault.

## 2. Logs and heap

An approved plain `GET /diag` reads the bounded in-RAM ring. No `verbose` or `clear` parameter is part
of this flow. The ring does not survive reboot; use existing Syslog for earlier BOOT/HEAP lines.
Use a separately approved serial fallback only if needed; production INFO builds omit raw DEBUG
GATT codes. Redacted status/log exports still need inspection before sharing; dumps remain private.

| Source | Meaning |
|---|---|
| `/status.sys` | Current INTERNAL free/minimum/largest bytes, always present |
| `/heap` | 24-hour tenths-of-KiB trend, null empty samples; `b_boot` divides prior runs; retained across powered resets |
| BOOT/HEAP log | Reset and allocation milestones; periodic `internal_largest` is watchdog evidence, plain 8BIT may include PSRAM |

A decreasing total and largest trend suggests leakage; stable total with falling largest suggests
fragmentation. Under 4 KiB continuously for five minutes can trigger the capped watchdog; OTA clears
the run. ARMED/countdown/recovered/OTA-cleared/EXHAUSTED/cap and next-BOOT breadcrumbs distinguish
its escalation from panic, brownout or WDT. A healthy current sample does not explain a past reboot.

## 3. Symptom-based handoff

| Evidence | Interpretation to verify | Proposed next scope |
|---|---|---|
| BLE failures, non-connectable advert | Connection limit/another holder; weak signal is another possibility | One board per car, range/other connection check; do not infer a firmware defect |
| `last_reboot:heap:<n>` | Sustained exhaustion was handled; repeated restarts do not solve its cause | Inspect prior heap/activity evidence; preserve watchdog limits |
| MQTT disconnected, uptime stable | Network or broker/TLS failure, not proof of crash | Inspect transport/error; never downgrade TLS silently |
| OTA invalid/signature-bad | Wrong authority is one possibility; corruption/chip/revision also need exclusion | Verified `$usb-recovery` only after matching evidence and separate approval |
| Boot loop before app_main after local flash | Unsigned app fails startup signature check | Signed-artifact recovery, separately authorized |
| Pairing became false | Deleted key, forced generation or changed VIN | Verify revocation/identity cause, then separately authorized re-pair; role-denied commands are not revocation |

## Result

Report exact evidence/version/target, confirmed versus suspected cause, missing history and proposed
next step. Never execute a table's remedy in this diagnostic scope. `$flash-esp32`/`$usb-recovery`
handle physical recovery; `$ship` handles authorized delivery; global `$tesla-key-e2e-evcc` handles
separately authorized cluster-to-car proof. Device health alone does not establish evcc E2E success.

`vehicle_data` is cache-only. Charge/climate/drive availability follows [API](../../../docs/API.md#vehicle-data):
recent command or live Charging/Starting contact opens the window; charge expires at 30 s and climate
at 300 s inside it. Missing post-reboot climate is expected until an awake-car rotation; heap-watchdog
boots seed no window. Reported odometer is required and otherwise last-known. An SMP `rc=6` alone
means disabled NimBLE Security Manager, not heap exhaustion; confirm context before diagnosing OOM.

Heap logs distinguish excused OTA allocation from recovery, the five-restart cap from success,
postponed OTA/identity restart from write-failed breadcrumb degradation. Re-derive their exact lines
in `heap_watchdog.hpp`/`vehicle_telemetry.cpp`; neither postponement nor later healthy heap proves a
previously blocked gate passed.
