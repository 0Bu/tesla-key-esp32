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
# wake_up, set_charging_amps (re-set current), set_charge_limit (change −10 then restore),
# door_lock/door_unlock (negative role test — must be REFUSED, see below)
RUN_COMMANDS=1 TIMEOUT=25 bash scripts/e2e_evcc.sh

# additionally charge_start / charge_stop — PHYSICALLY commands the car to start/stop charging
RUN_COMMANDS=1 ALLOW_CHARGE_TOGGLE=1 TIMEOUT=25 bash scripts/e2e_evcc.sh

# full command smoke test — additionally exercises EVERY remaining firmware command
# (charge_port open/close, flash_lights, honk_horn, climate start/stop, sentry on/off,
# scheduled_charging on/off). PHYSICALLY actuates the car; NOT part of the evcc path.
RUN_COMMANDS=1 RUN_ALL_COMMANDS=1 TIMEOUT=25 bash scripts/e2e_evcc.sh
```

The script does **not** hardcode any real device address or vehicle identifier — it learns them from the firmware itself (the single source of truth), so nothing private is committed to the repo:

- **`ESP32_URL`** defaults to the device's mDNS name **`http://tesla-key-esp32.local`** (advertised by the firmware in `main.cpp`), resolvable from the evcc pod on the same LAN. Override it with the board's IP if mDNS isn't reachable in your cluster.
- **`VIN`** is left unset by default and **auto-discovered from `GET $ESP32_URL/status`** (the `"vin"` field the firmware reports). Set `VIN=…` only to pin a specific value.

Other overrides (env vars; fall back to the built-in defaults if unset): `EVCC_NS` (default `default`), `ITER` (vehicle_data burst size), `TIMEOUT` (per-request seconds). The test is **target-agnostic** — point `ESP32_URL` at whichever esp32 / esp32s3 / esp32c3 / esp32c6 / esp32c5 board you want to verify and the VIN follows automatically from that board's `/status`.

## Before running the write path — ask first

`charge_start` / `charge_stop` are outward-facing physical actions on a real vehicle. Do **not** enable `ALLOW_CHARGE_TOGGLE=1` unless the user explicitly opted in. The same goes for `RUN_ALL_COMMANDS=1`, which additionally flashes the lights, honks the horn, opens the charge-port flap, toggles climate/sentry, etc. — only run it when the user explicitly wants a full command smoke test (e.g. just after a flash). `set_charge_limit` changes the car's limit but the script restores it; report the baseline it captured. `door_lock`/`door_unlock` are safe — the car refuses them for the Charging-Manager role (that's exactly what the test asserts).

## What the script checks

1. `GET /status` + `GET /api/proxy/1/version` — device up, `paired:true`, BLE connected, firmware version, the read-only `tele` telemetry block present (background poll alive), the `/api/proxy/1/version` endpoint (part of the proxy API surface — the firmware web UI/OTA read it; **evcc itself does not call it**), and that the two version sources agree (`/status` = `X`, proxy = `X-esp32` — catches a half-applied OTA). The `-esp32` suffix in the proxy `version` is **fixed for all five targets**; the actual chip is reported separately in `platform` (`ESP32` / `ESP32-S3` / `ESP32-C3` / `ESP32-C6` / `ESP32-C5` since the multi-target build), so don't expect e.g. `X-esp32c6` here — the script checks `platform` for presence only.
2. `GET vehicle_data?endpoints=charge_state` × `ITER` — **the critical no-timeout check.** Reports avg/max latency, a transport-failure count, and a separate *stale* count (well-formed `result:false` while the car sleeps — evcc-safe), and asserts every field evcc parses is present (`charging_state`, `battery_level`, `charge_limit_soc`, `charger_power`, `charge_rate`, `charge_amps`, `battery_range`).
3. `GET body_controller_state` — live VCSEC BLE read (lock/sleep/presence).
4. Commands (gated by `RUN_COMMANDS=1`) — `wake_up`, `set_charging_amps`, `set_charge_limit` (change+restore), `door_lock`/`door_unlock` (**inverted** assertion: must be refused → confirms the Charging-Manager role boundary), and optionally `charge_start`/`charge_stop` (`ALLOW_CHARGE_TOGGLE=1`).
4b. Extended sweep (gated by `RUN_ALL_COMMANDS=1`) — every remaining firmware command: `charge_port_door_open/close`, `flash_lights`, `honk_horn`, `auto_conditioning_start/stop`, `set_sentry_mode`, `set_scheduled_charging`. Each is "soft" (car-side `false` is a NOTE, not a FAIL) — the point is that the whole command surface dispatches and round-trips without faulting the proxy.
5. evcc's own logs (last 30 min) — scanned for Tesla timeout/`deadline`/`canceled`/`refused`/`i/o` errors.

Exit 0 + `✅ e2e OK` means evcc can drive the ESP32 with no timeouts.

## Coverage map (test ↔ firmware surface)

What this test deliberately does **not** touch, and why — so "e2e OK" is not mistaken for "every endpoint verified":

- **Management / destructive endpoints are out of scope:** `/scan`, `/gen_keys`, `/send_key`, `/set_time`, `/set_vin`, `/set_mqtt`, `/ota/check`, `/ota/update`, `/ota/status`, and the `/` web UI. They re-pair, reboot, or wipe NVS — not part of the evcc runtime path. Verify those manually or via the flash/OTA skills. (`/set_mqtt` *is* a real route — `handle_set_mqtt` in `http_config.cpp`, POST `{"broker":"host:port"}` → persist + reboot; it's excluded here because it reboots, not because it's missing.)
- **`/mcp`** (the MCP JSON-RPC server for AI agents, `main/mcp_server.cpp`) is not part of the evcc runtime path and is not exercised here — smoke-test it separately per `docs/MCP.md`.
- **`/diag`** is used reactively (see *Diagnosing a real failure*), not asserted.
- Everything the firmware *dispatches* — all 15 commands, `vehicle_data`, `body_controller_state`, `/status`, `/api/proxy/1/version` — is reachable by the test once the right gate is set (`RUN_COMMANDS` / `ALLOW_CHARGE_TOGGLE` / `RUN_ALL_COMMANDS`).

## Interpreting results

- **Reads must never time out.** `vehicle_data` is served from the firmware's `last_known_charge_` cache (refreshed out-of-band while the BLE link is warm), so latency is ~0–3 ms even while a real BLE poll runs concurrently. A timeout here is a genuine problem — check device reachability and that BLE is connected (`/status`), and look at `GET /diag?verbose=1` on the device.
- **`charge_start`/`charge_stop` returning `false` is usually NOT a bug.** The car rejects them based on live charging state — e.g. a vehicle at its limit / "Complete" refuses `charge_start` (`/diag` shows `Infotainment action failed: complete`). The script flags these as `NOTE`, not `FAIL`. They only truly pass when the car is plugged in and below its limit.
- **`door_lock`/`door_unlock` are an *inverted* test — a car-side refusal is the PASS.** The key is enrolled Charging-Manager-only, so the car must reject them; `result:true` is a hard FAIL (the key would carry owner privileges — a security regression). Subtlety: the firmware answers `result:false` *even when the car is unreachable* (reason `"vehicle not reachable"`), so the test only treats a `result:false` with a **non-reachability** reason as the PASS; a reachability reason (or a transport timeout) is a FAIL "can't confirm — re-run with the car awake". So this assertion is only meaningful when the car is awake and signing (the `wake_up` issued first in `RUN_COMMANDS` nudges it).
- **Extended-sweep (`RUN_ALL_COMMANDS`) commands returning `false` is usually NOT a bug** — same reasoning as charge_start/stop: acceptance depends on live state. The sweep proves the command path round-trips, not that the car acted.
- evcc polls **only** `vehicle_data` on a loop (never `body_controller_state`), so routine operation never hits the blocking BLE read path.

## Diagnosing a real failure

```bash
POD=$(kubectl get pod -n default -l app=evcc -o jsonpath='{.items[0].metadata.name}')
# what evcc itself sees
kubectl logs -n default "$POD" --since=30m | grep -iE 'tesla|vehicle_data|timeout|deadline'
# device-side BLE command/result log
kubectl exec -n default "$POD" -- wget -qO- 'http://<ESP32-IP>/diag?verbose=1'
```
