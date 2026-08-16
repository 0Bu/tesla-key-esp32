---
name: vehicle-command-audit
description: Read-only three-way conformance audit of tesla-key-esp32 against teslamotors/vehicle-command and the capabilities of the pinned yoziru/tesla-ble. Report upstream/local/feasibility findings; do not edit code/docs/dependencies, publish, or contact a vehicle unless the user separately authorizes implementation.
---

> **Canonical runner-neutral skill.** Read [`AGENTS.md`](../../../AGENTS.md) before acting.
> Project skills are canonical under [`.agents/skills/`](../), and lifecycle/PR policy is
> enforced by the runner-neutral core under [`tools/agent-hooks/`](../../../tools/agent-hooks/).
> This skill does not grant permissions beyond the user's explicit request.
> Invoke this workflow canonically as `$vehicle-command-audit`.

# vehicle-command-audit — upstream conformance audit against teslamotors/vehicle-command

This firmware is a **BLE↔HTTP proxy for a Tesla**, built on **yoziru/tesla-ble** (a C++ port),
which itself mirrors Tesla's official Go SDK **teslamotors/vehicle-command**. `$vehicle-command-audit`
checks the firmware against that authoritative upstream reference and reports drift.

> **Read-only boundary.** An audit request authorizes source retrieval, repository inspection, and a
> report only. Do not edit code/docs/dependencies, create tracking items, commit, push, merge,
> release, flash, OTA, or contact/wake/command a vehicle. Any accepted fix is a separately
> authorized implementation task.

It is the *external-conformance* counterpart to [`$project-review`](../project-review/SKILL.md)
(internal coherence). project-review asks "do our code/docs/config/build agree with **each
other**?"; this skill asks "do they agree with **Tesla's protocol**, and is each proposed fix
**physically possible** with the library we link?"

## The one rule that makes this skill different — the **tesla-ble feasibility gate**

`teslamotors/vehicle-command` is the **truth**, but it is NOT what we run. We run
**yoziru/tesla-ble** (pinned in [`main/idf_component.yml`](../../../main/idf_component.yml) — **read
the pin first**, currently `v5.1.1`). The Go SDK exposes commands, builders, fields and enum
values the pinned C++ port may **not** have. So every comparison is **three-way**, and a
divergence from upstream does **not** automatically mean "change the code":

```
1. UPSTREAM truth  — does teslamotors/vehicle-command say/do X?
2. LOCAL behaviour — does the firmware do X?
3. FEASIBILITY     — CAN tesla-ble <pin> even do X? (builder exists? enum value exists? API supports it?)
```

Decision table:

| upstream says X | we do X | tesla-ble can do X | → correct action |
|---|---|---|---|
| yes | no | **yes** | **recommend a code fix** to match upstream, including every cross-cutting sink |
| yes | no | **no** | **do not recommend local protocol code**; report a doc-note proposal and/or separately authorized library-bump tracking item. A missing builder cannot be called safely. |
| yes | yes | yes | conformant — verify the docs *describe* it correctly |
| no  | yes | — | we do *more* than upstream (e.g. a clamp). Fine if safe + documented; flag only if it hides an error the caller should see |

**Second reality gate — the Charging-Manager role.** This device enrols a **Charging-Manager
key only** (charging + wake). Many commands the firmware *sends* are **rejected by the car** for
that role. "tesla-ble has the builder" and "upstream lists the command" do **not** mean the
command *works here*. A doc that implies a role-denied command works is a real finding even when
the code and the builder are perfectly fine. (Worked example: the `flash_lights`/`honk_horn`/
`sentry`/`climate` doc drift below.)

**Net effect for this project:** the implementation tracks the protocol *well*. In practice
almost every real finding lands as **documentation**, and almost every code "fix" you're tempted
to make is blocked by gate 3 (tesla-ble can't) or gate "role" (car rejects). Recommend a code change only when all three gates clear. **Never hand-edit or commit
`managed_components/`** — propose
a pin bump or a guard at our own call boundary. For a confirmed defect inside library dispatch
with no fixed upstream release, propose a minimal repository patch under `patches/tesla-ble/`,
to be implemented separately and applied deterministically to the materialised managed-component
tree, then explicitly rebased on pin bumps.

## Sources — what to read, and how to fetch it

### Upstream truth — `teslamotors/vehicle-command`
Fetch **raw** files (base `https://raw.githubusercontent.com/teslamotors/vehicle-command/main`).
The high-value paths (verified to exist):

| Dimension | Upstream file(s) |
|---|---|
| BLE transport (UUIDs, name, framing, MTU, max-conns) | `pkg/connector/ble/ble.go`, `pkg/connector/ble/errors.go` |
| Session / signing / anti-replay / clock | `pkg/protocol/protocol.md`, `internal/authentication/signer.go`, `pkg/protocol/protobuf/signatures.proto` |
| Roles | `pkg/protocol/protobuf/keys.proto` (Role enum), `pkg/protocol/protocol.md` (role *scope* prose) |
| Wake / sleep / body controller | `pkg/vehicle/vcsec.go`, `pkg/vehicle/state.go`, `pkg/vehicle/vehicle.go`, `pkg/protocol/protobuf/vcsec.proto` |
| Commands + params/ranges | `pkg/vehicle/charge.go` (**note: `charge.go`, not `charging.go`**), `pkg/vehicle/climate.go`, `cmd/tesla-control/commands.go` |
| Error / fault taxonomy | `pkg/protocol/protobuf/universal_message.proto` (`MessageFault_E`, `OperationStatus_E`), `pkg/protocol/protobuf/car_server.proto` |

### Feasibility — `yoziru/tesla-ble` at the pinned tag
Fetch raw at the **pin** (base `https://raw.githubusercontent.com/yoziru/tesla-ble/<pin>`; confirm
`<pin>` from `idf_component.yml`). Layout at v5.1.1:
`include/{vehicle.h, client.h, command_error.h, message_builders.h, peer.h, vin_utils.h, errors.h, …}`
and `src/{vehicle.cpp, client.cpp, peer.cpp, message_builders.cpp, message_processor.cpp, crypto_context.cpp, vin_utils.cpp, errors.cpp, …}`.
- **Does a command builder exist?** → `src/message_builders.cpp` (e.g. `scheduledChargingAction` IS
  registered; `scheduledDepartureAction` is **not** — that absence is *why* scheduled departure isn't exposed).
- **Enum / API values** (`SleepState`, `WakePolicy`, roles, form factors) → `include/vehicle.h`, `include/client.h`.
- **How a fault becomes a string** the firmware matches → `include/command_error.h`, `src/vehicle.cpp`.
- **VIN→BLE-name / matching** → `include/vin_utils.h`, `src/vin_utils.cpp` (the firmware delegates here; it does **not** build the name itself).

### Local — the firmware + its four docs
Code: `main/ble_client.{cpp,hpp}`, `main/vehicle_ctrl.{cpp,hpp}` (+ `vehicle_commands.cpp`,
`vehicle_telemetry.cpp`, `vehicle_pairing.cpp`), `main/http_server.cpp` (+ `http_api.cpp`,
`http_status.cpp`, `http_ota.cpp`, `http_config.cpp`), `main/mqtt_ha.cpp`,
`main/ota_update.cpp`, `main/Kconfig.projbuild`, `partitions.csv`.
Docs to hold accountable: [`README.md`](../../../README.md),
[`docs/README.md`](../../../docs/README.md), [`docs/SECURITY.md`](../../../docs/SECURITY.md),
[`docs/ARCHITECTURE.md`](../../../docs/ARCHITECTURE.md), and
[`docs/MCP.md`](../../../docs/MCP.md). `AGENTS.md` owns runner policy and safety boundaries, not
the command, link-state or pairing catalogs audited here.

### Fetch gotchas (these silently waste a pass if you don't know them)
- Use **`raw.githubusercontent.com`**, not `github.com/.../blob/...` — the HTML blob view **403s** via a webpage fetcher.
- **GitHub `issues`/`pulls` and `developer.tesla.com` also 403** via a webpage fetcher → use **web search**, or `gh api` for GitHub.
- Tesla **role scope is prose, not protobuf** — `keys.proto` lists the Role enum with **no
  permission comments**. The authority for "what may a Charging Manager run" is
  `pkg/protocol/protocol.md` ("a Charging Manager can read vehicle data and authorize commands
  that affect vehicle charging") plus Fleet-API docs — search for those.

## The dimensions to compare (checklist + current baseline)

Walk all of these. The **baseline verdict** is what a prior full audit established — your job is to
re-confirm it against the *current* tree and catch anything that drifted since. A baseline of
"matches" is not a licence to skip; it's the assertion to re-test.

1. **BLE transport** — service `00000211…`, write `…0212`, notify `…0213`; VIN→name `S%02xC` over
   the **first 8 bytes** of `sha1(VIN)` (⇒ `S` + 16 hex + `C`); 2-byte big-endian length prefix;
   block `min(txMtu,maxBLEMessageSize)-3`; `ErrMaxConnectionsExceeded` keyed off the advert
   **`Connectable`** flag (not the connect error). *Baseline: matches.* (We use a fixed 20-byte
   write chunk vs upstream's MTU-derived block — slower but safe; OK as long as docs say so.)
2. **Roles / Charging-Manager scope** — charging commands + wake only; everything else
   (lights/horn/sentry/**climate**/locks) is role-rejected. *Baseline: code correct
   (`vehicle_commands.cpp` role-refusal comment), **docs understated** — see worked example.*
3. **Session / signing / clock** — `expires_at` = **vehicle's `SessionInfo.ClockTime` + a
   monotonic `steady_clock` delta** (`src/peer.cpp` `generate_expires_at`), per-domain counter +
   16-byte epoch, separate VCSEC/Infotainment sessions. **The device wall clock does NOT enter
   per-command signing/expiry** — a wrong RTC cannot make an already-loaded command stale or
   replayable. It **does** gate persisted-session reuse: `Vehicle::load_session()` computes
   `system_clock - SessionInfo.ClockTime` and rejects age > 1 h, so the NVS clock restore before
   controller init is required. *Baseline: code and current comments match this split.*
4. **Pairing / whitelist** — add-key carries role + `KEY_FORM_FACTOR_CLOUD_KEY`, no key name (car
   shows "Unknown key"), requires an **NFC card on the console reader**, verify via a SessionInfo
   probe. *Baseline: matches.* (Note: the **"3"** is the simultaneous-BLE-**connection** limit; a
   car stores up to **19 keys** — don't call it a 3-key cap.)
5. **Command params / ranges** — amps clamp **0–48** (faithful to real chargers; upstream has no
   cap), charge-limit **50–100**, scheduled-charging `start_minutes` **0–1439** after local
   midnight. *Baseline: code faithful; docs say amps "0–32".*
6. **Wake / sleep** — wake is a **VCSEC-domain** `RKE_ACTION_WAKE_VEHICLE` (not infotainment);
   `vehicleSleepStatus`/`vehicleLockState`/`userPresence` string values match the upstream enums;
   the "trust debounced VCSEC ASLEEP, never VCSEC AWAKE" asymmetry is a correct reading.
   *Baseline: matches; one stray "wake … infotainment-only" comment is imprecise.*
7. **Error / fault taxonomy + revocation heuristic** — `OperationStatus`, `MessageFault`
   (`UNKNOWN_KEY_ID=3`, `INACTIVE_KEY=4`, `INSUFFICIENT_PRIVILEGES=7`, `INVALID_KEY_HANDLE=27`),
   `SESSION_INFO_STATUS_KEY_NOT_ON_WHITELIST`. Confirm the firmware's three detectors stay sound:
   the `set_message_callback` fault match (the path that actually fires on a **cached** session),
   the `"whitelist"` substring (session-info handshake only), and the two-strike `"authentication
   failed"` **gated to the health probe** so a role-denied user command can't destroy a pairing.
   *Baseline: sound; `docs/ARCHITECTURE.md` describes all three detectors.*
8. **Library-version claims** — every command the firmware calls resolves to a real builder at the
   pin; doc claims about what's *not* exposed (scheduled departure) match the pin's
   `message_builders.cpp`. *Baseline: matches at v5.1.1.*
9. **evcc / TeslaBleHttpProxy HTTP shape** — `/api/.../command/{name}` names, `vehicle_data` =
   `.response.response.charge_state.*` with **`charge_amps`** (not `charging_amps`), doubled
   `response`, **miles/mph on the `/api` path** (metric is MQTT-only), `charging_state` strings
   `Charging/Disconnected/Complete/Stopped/NoPower/Starting`. *Baseline: full match.*
9a. **Response-counter anti-replay** — upstream `yoziru/tesla-ble` v5.1.1 logs a failed
    `validate_response_counter()` but continues dispatch. Verify the repository patch under
    `patches/tesla-ble/` still returns before state callbacks and FIFO completion, applies to
    the managed dependency tree, and is rebased explicitly on every pin bump.
10. **Docs internal coherence vs code** — `/status.link` and MQTT `sleep_status` enum value sets,
    endpoint/CONFIG/partition/version drift across the four docs. *Baseline: a few enum-set
    omissions — see worked examples.*

## Worked findings from the last full audit (institutional memory)

These were previously confirmed and are institutional examples. Re-verify each against current
sources; if still present, report it rather than editing. Nearly all are documentation findings.

> **Status (re-verified 2026-07-02, project-review pass): findings 1–8 are FIXED** in the
> current tree — the docs/comments now state the code facts each row cites. Only **#9** (the
> silent `set_charge_limit` `<50→50` clamp reporting success, `vehicle_commands.cpp`) remains, and
> it is the optional, low-severity row. The table stays as institutional memory for the *shape*
> of findings; re-verify a row against the tree before acting on it.

| # | Cat | Where (doc) | Drift | Fix |
|---|---|---|---|---|
| 1 | doc (crux) | `docs/README.md` "Commands" | Claims **only** `door_lock/unlock` are role-rejected. Also rejected: `flash_lights`, `honk_horn`, `set_sentry_mode`, `auto_conditioning_start/stop`. Code already names them at the `vehicle_commands.cpp` role-refusal comment. | Widen the "sent but rejected for the Charging-Manager role" note to the full set; only charging + charge-port + wake actually execute. |
| 2 | doc | `docs/README.md` `/status.link` and MQTT `sleep_status`; `docs/ARCHITECTURE.md` "Sleep / link-state" | Enum value sets omit **`idle`/`IDLE`** (code emits 5 / 4 values). | Add `idle`/`IDLE` to the owning deep references. |
| 3 | doc | `docs/README.md:126` | `set_charging_amps` "(0–32)" but code clamps **0–48**. | Document `(0–48; car enforces its per-model max)`. |
| 4 | doc | `README.md:54` | Quotes car prompt as "Add new key"; firmware strings say **"Add key"**. | One-word fix. |
| 5 | doc | `README.md` "Pair with the car"; `docs/README.md` "Pairing" | "max 3 **keys** per vehicle" — really ~3 simultaneous **connections** (19 keys stored). | Reframe as the ~3-connection limit. |
| 6 | doc/comment | `Kconfig.projbuild` OTA help | Says image is `tesla-key-esp32-<target>.bin`; actual suffix scheme is `""`/`-s3`/`-c3`/`-c6`. | Correct to the real suffix map. |
| 7 | doc/comment | old `vehicle_ctrl.hpp` / `main.cpp` / `http_server.cpp` comments | Historical comments said the device made no NTP call and conflated command expiry with wall time. Current contract: SNTP is primary, `/set_time` is fallback; wall time gates TLS/human timestamps **and persisted-session load age**, while command `expires_at` uses vehicle clock + monotonic delta. | Keep that distinction; do not remove the early NVS clock restore as "signing does not use RTC". |
| 8 | doc | `docs/ARCHITECTURE.md` "Pairing lifecycle / invalidation" | Describes only the `"whitelist"` substring; omits the **primary** `UNKNOWN_KEY_ID` `set_message_callback` detector that fires on a cached session. | List all three detectors, primary first. |
| 9 | logical (low) | `vehicle_commands.cpp:227-230` | `set_charge_limit` silently clamps `<50→50` and reports **success** (upstream passes through and lets the car answer). Harmless on the evcc path. | Optional: reject out-of-range, or pass through. |

## How to run

1. **Confirm the pin** (`idf_component.yml`) — every feasibility check is against *that* tesla-ble
   version, not "latest". If the pin changed since the table above, re-verify dimension 8 first.
2. **For each dimension:** fetch the upstream truth (raw URL), fetch the tesla-ble capability at the
   pin, read the local code + the doc that describes it. Apply the **three-way gate** + the
   **role gate**. Classify each divergence: `technical` (mis-implements the protocol) /
   `logical` (internally wrong) / `documentation` (a doc claim that's false/misleading/drifted) /
   `gap` (something expected is missing/unhandled).
3. **Verify before asserting (refute by default).** Firmware is easy to mis-diagnose and **the Go
   SDK is not the code we run** — confirm a claim against **tesla-ble at the pin**, not just
   vehicle-command. Separate **confirmed** from **suspected**. A "we diverge from upstream" claim is
   only real after gate 3 says the divergence is even fixable here.
4. **Recommend the smallest safe correction.** Prefer a documentation proposal where the code is
   conformant. Recommend code only when all three gates clear, and enumerate every cross-cutting
   sink from `$project-review`. Never edit generated dependency trees during the audit.
5. **Write the report** (structure below). State explicitly that no fixes were applied.

## Report structure

```
# vehicle-command audit — tesla-key-esp32 (<date>)  ·  tesla-ble pin: <pin>

## Summary
<overall conformance; counts by category/severity; the headline (usually: impl faithful, doc drift).>

## Findings   (priority order)
### [CATEGORY/SEV] <title>
- Upstream truth: <fact> — `<vehicle-command path>` (quote)
- tesla-ble <pin>: <can / cannot do it> — `<tesla-ble path>` (the feasibility gate result)
- Local: `<file:line>` (code) and/or `<doc:line>` (the claim)
- Verdict: technical | logical | documentation | gap · confirmed | suspected
- Proposed fix: <doc edit | code edit only if all gates clear | library-bump tracking item>, including every place that must move together

## Conformant (re-verified, no change)
<the dimensions that match upstream — name them, so "no finding" is an asserted result, not a gap in coverage.>

## Blocked by tesla-ble <pin> (cannot fix in code)
<divergences from upstream that the pinned library can't express — documented as such, optionally a pin-bump item.>

## Prioritized actions
1. <must-fix> … 2. <should-fix> … 3. <nice-to-have> …
```

Order by user impact: a role/protocol mismatch that misleads a user or breaks evcc outranks a
comment nit. Keep findings tight and actionable — someone should be able to fix the project from
the report without re-deriving the upstream context.
