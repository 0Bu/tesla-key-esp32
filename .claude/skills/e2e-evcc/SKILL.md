---
name: e2e-evcc
description: Run the end-to-end test of the evcc → tesla-key-esp32 → vehicle path. Use when asked to verify the live integration, check that evcc can reach the ESP32 without timeouts, confirm all evcc functions work against the current firmware, or smoke-test the device after a flash/OTA. Runs scripts/e2e_evcc.sh from inside the evcc pod (the real network path).
---

# e2e-evcc — live integration test

Verifies the whole chain evcc actually uses: **evcc pod (k3s) → `tesla-ble` HTTP template → ESP32 (`tesla-key-esp32`) → BLE → the real car.** The test runs the *exact* HTTP calls evcc makes, from **inside the evcc pod**, so it exercises the true path (pod → k3s node → LAN → ESP32 → BLE), not a shortcut from the laptop.

The driver script is [`scripts/e2e_evcc.sh`](../../../scripts/e2e_evcc.sh). It needs `kubectl` pointed at the cluster where evcc runs.

## How to run

Always start read-only (safe, this is the part that matters for evcc):

```bash
bash scripts/e2e_evcc.sh
```

The write path issues real signed-BLE commands to the car — only run it when the user has asked to test commands, and confirm scope first (see below):

```bash
# wake_up, set_charging_amps (re-set current), set_charge_limit (change −10 then restore)
RUN_COMMANDS=1 TIMEOUT=25 bash scripts/e2e_evcc.sh

# additionally charge_start / charge_stop — PHYSICALLY commands the car to start/stop charging
RUN_COMMANDS=1 ALLOW_CHARGE_TOGGLE=1 TIMEOUT=25 bash scripts/e2e_evcc.sh
```

Useful overrides (auto-discovered from the evcc DB if unset): `ESP32_URL`, `VIN`, `EVCC_NS` (default `default`), `ITER` (vehicle_data burst size), `TIMEOUT` (per-request seconds).

## Before running the write path — ask first

`charge_start` / `charge_stop` are outward-facing physical actions on a real vehicle. Do **not** enable `ALLOW_CHARGE_TOGGLE=1` unless the user explicitly opted in. `set_charge_limit` changes the car's limit but the script restores it; report the baseline it captured.

## What the script checks

1. `GET /status` — device up, `paired:true`, BLE connected, firmware version.
2. `GET vehicle_data?endpoints=charge_state` × `ITER` — **the critical no-timeout check.** Reports avg/max latency and a timeout count, and asserts every field evcc parses is present (`charging_state`, `battery_level`, `charge_limit_soc`, `charger_power`, `charge_rate`, `charge_amps`, `battery_range`).
3. `GET body_controller_state` — live VCSEC BLE read (lock/sleep/presence).
4. Commands (gated) — `wake_up`, `set_charging_amps`, `set_charge_limit` (change+restore), and optionally `charge_start`/`charge_stop`.
5. evcc's own logs (last 30 min) — scanned for Tesla timeout/`deadline`/`canceled`/`refused`/`i/o` errors.

Exit 0 + `✅ e2e OK` means evcc can drive the ESP32 with no timeouts.

## Interpreting results

- **Reads must never time out.** `vehicle_data` is served from the firmware's `last_known_charge_` cache (refreshed out-of-band while the BLE link is warm), so latency is ~0–3 ms even while a real BLE poll runs concurrently. A timeout here is a genuine problem — check device reachability and that BLE is connected (`/status`), and look at `GET /diag?verbose=1` on the device.
- **`charge_start`/`charge_stop` returning `false` is usually NOT a bug.** The car rejects them based on live charging state — e.g. a vehicle at its limit / "Complete" refuses `charge_start` (`/diag` shows `Infotainment action failed: complete`). The script flags these as `NOTE`, not `FAIL`. They only truly pass when the car is plugged in and below its limit.
- evcc polls **only** `vehicle_data` on a loop (never `body_controller_state`), so routine operation never hits the blocking BLE read path.

## Diagnosing a real failure

```bash
POD=$(kubectl get pod -n default -l app=evcc -o jsonpath='{.items[0].metadata.name}')
# what evcc itself sees
kubectl logs -n default "$POD" --since=30m | grep -iE 'tesla|vehicle_data|timeout|deadline'
# device-side BLE command/result log
kubectl exec -n default "$POD" -- wget -qO- 'http://<ESP32-IP>/diag?verbose=1'
```
