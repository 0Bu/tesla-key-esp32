# MCP integration guide

The device is an MCP ([Model Context Protocol](https://modelcontextprotocol.io/)) server:
any MCP-capable client — Claude Desktop, Claude Code, VS Code, or an agent you build with
the MCP SDKs — can read the vehicle's cached state and drive the charging command set
directly, with no proxy process in between. The endpoint runs on the same `esp_http_server`
as the REST API and web UI, on port 80.

This is the user/integrator guide: wire format, tool reference, client configuration and
troubleshooting. For the firmware-internal design (request/reply bounds, ownership and stateless handling)
see [`ARCHITECTURE.md`](ARCHITECTURE.md#mcp-endpoint-mcp); for the security posture see
[`SECURITY.md`](SECURITY.md#http-api-exposure).

> **Trusted LAN only.** Like every other endpoint on this device, `/mcp` has no
> authentication and no TLS — anyone on the network can call it. The enrolled key is
> Charging Manager only (charging + wake; it cannot unlock or drive the car), and the
> endpoint grants nothing the open REST API doesn't already expose. Never port-forward it.

---

## Endpoint & transport

```
POST http://<ESP32-IP>/mcp        (or http://tesla-key-esp32.local/mcp)
Content-Type: application/json
```

The server implements the **Streamable HTTP transport in its stateless profile**:

| Property | Behavior |
|----------|----------|
| Request | Exactly **one JSON-RPC 2.0 message per POST** (max body 2 KB) |
| Response | JSON-RPC handler replies use `application/json`; notifications have no body. Shared admission/exception fallbacks are described below; no SSE stream is opened |
| `GET /mcp` | `405 Method Not Allowed` (+ `Allow: POST`) — no server-initiated stream is offered |
| Sessions | None. No `Mcp-Session-Id` header is issued or expected; every request is self-contained |
| `MCP-Protocol-Version` header | Ignored (nothing version-dependent happens after `initialize`) |
| `jsonrpc` member | Required on requests **and notifications** as the exact string `"2.0"`. Missing, non-string and other-version values are rejected with `-32600` before notification handling or method dispatch |
| Notifications (a valid JSON-RPC 2.0 message with a string `method` but no `id`) | `202 Accepted`, empty body, without dispatching the named method. This includes id-less `tools/call`, not just `notifications/*`. A message with neither `method` nor `id` is malformed → `-32600` |
| JSON-RPC request `id` | A string of at most 64 bytes, or a canonical decimal JSON-safe integer in `[-9007199254740991, 9007199254740991]`. Numeric fractions, exponent notation, negative zero, non-finite/materialization artifacts and values at or beyond `±2^53` are rejected with `-32600`; accepted numeric IDs are emitted as the same exact decimal integer. Explicit null, other types, longer strings and escaped U+0000 are also invalid and correlate as `id:null` |
| Duplicate object keys | Rejected recursively with `-32600`, including decoded equivalents such as `method` and `\u006dethod`. A separate unique valid `id` is preserved for the error; a duplicate/invalid `id` is ambiguous and returns `id:null` |
| JSON-RPC batches (arrays) | Rejected with `-32600` (batching was removed in protocol `2025-06-18`) |
| Protocol revisions | `2025-06-18` and `2025-03-26`. Requesting anything else returns the server's latest (`2025-06-18`); the client may then disconnect per the spec |

Clients typically send `Accept: application/json, text/event-stream` — that is fine; the
spec lets the server pick either representation and this one always answers plain JSON.

### Supported methods

| Method | Reply |
|--------|-------|
| `initialize` | Capabilities (`tools` only — no resources, prompts or logging), server info, instructions |
| `notifications/initialized` (and any other notification) | HTTP `202`, no body |
| `ping` | `{}` |
| `tools/list` | The 9 tools below with their JSON schemas |
| `tools/call` | Tool result (`content` + `isError`) |
| anything else (`resources/list`, …) | JSON-RPC error `-32601` |

---

## Tools

The tool set mirrors **exactly the commands the enrolled Charging-Manager key can
execute** — plus one read-only state tool. Commands the car refuses for this key role
(doors, climate, horn, sentry) are deliberately not exposed: a tool that always fails
only misleads the calling model.

| Tool | Arguments | Behavior |
|------|-----------|----------|
| `get_vehicle_state` | — | Returns cached state as JSON text. **Never touches BLE** — no scan, no connect, no wake — so an agent may poll it freely without keeping the car awake. |
| `wake_up` | — | Sends a BLE wake and confirms fresh Infotainment evidence within the command deadline; [HTTP semantics](API.md#commands) also define definite and uncertain failures. |
| `charge_start` | — | Start charging. |
| `charge_stop` | — | Stop charging. |
| `charge_port_open` | — | Open the charge port door. |
| `charge_port_close` | — | Close the charge port door. |
| `set_charging_amps` | `amps` (integer, 0–48, required) | Set the charging current. |
| `set_charge_limit` | `percent` (integer, 50–100, required) | Set the charge limit. |
| `set_scheduled_charging` | `enable` (boolean, required), `start_minutes` (integer, 0–1439) | Daily scheduled-charging start time, minutes after local midnight. |

Notes that matter to a calling agent:

- **Command latency:** commands are synchronous and block for the BLE round-trip. The
  first command after idle typically takes **~3–5 s** (BLE scan + connect); an
  unreachable car holds the request for the full command timeout (**up to 20 s**) before
  the `isError` result comes back. Give your MCP client a request timeout of at least 30 s.
- **Missing required arguments are rejected** with JSON-RPC `-32602`
  (`missing required argument: <key>`) — a call like `set_scheduled_charging` without
  `enable` errors instead of guessing. A **present but unparseable** argument (e.g.
  `start_minutes: "08:00"`) is likewise `-32602`, never silently defaulted. Canonical decimal
  strings are accepted for integers (`"16"` → 16); whitespace, plus signs, leading zeroes, negative
  zero, hexadecimal, exponent and fractional string forms are rejected. Integral JSON numbers
  (including `1e1`) and numeric `0`/`1` for booleans retain their existing checks. Fractional or out-of-range integers are
  rejected rather than silently executing a different value (the schemas advertise the same
  bounds — both are generated from one spec table, so they cannot drift).
- **Failures are tool results, not protocol errors:** a refused or undeliverable command
  comes back as a normal `tools/call` result with `isError: true` and the reason as text —
  the car's refusal reason when it answered, or `vehicle not reachable` when it didn't. JSON-RPC errors are
  reserved for malformed requests (unknown tool/method, missing required args, parse
  errors). Already-satisfied charging requests follow the [shared command policy](API.md)
  and return successful tool results.
- **`get_vehicle_state` is cache-fed.** Fields appear only when known: after a reboot
  with the car asleep, expect `vin`, `paired`, `session_available`, `charge_available:false` and `link`.
  `paired` means a stored VCSEC session; `session_available` means a usable VCSEC session in RAM
  for the current identity. Neither proves reachability or fresh vehicle acceptance. `link` is the same value
  the web UI hero uses — one of `awake` / `asleep` / `idle` / `unreachable`, plus
  `unknown` before first contact (see
  [`ARCHITECTURE.md`](ARCHITECTURE.md#sleep--link-state-the-single-source-of-truth)) —
  and `last_seen_s` is seconds since the last live contact. Optional telemetry fields
  (`soc`, `usable_soc`, `charging_state`, `charge_limit`, `charge_amps`, `charger_power_kw`)
  appear only when reported by the vehicle and `charge_available` is true. This flag uses the
  same cache-only charge availability gate as REST: valid last-known idle data is available;
  inside the active window a sample older than 30 s is unavailable. Unavailable charge data is
  omitted without polling, connecting or waking the vehicle.

---

## Wire examples (curl)

The endpoint is plain HTTP + JSON, so everything is testable with curl.

### 1. `initialize`

```bash
curl -s http://tesla-key-esp32.local/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{
        "protocolVersion":"2025-06-18",
        "capabilities":{},
        "clientInfo":{"name":"curl","version":"0"}}}'
```

```json
{"jsonrpc":"2.0","id":1,"result":{
  "protocolVersion":"2025-06-18",
  "capabilities":{"tools":{}},
  "serverInfo":{"name":"tesla-key-esp32","version":"1.5.0"},
  "instructions":"BLE-to-HTTP bridge for one Tesla, paired as Charging Manager: charging commands and cached read-only state only. get_vehicle_state never wakes the car; commands block for the BLE round-trip — typically 3-5s after idle, up to 20s when the car is unreachable."}}
```

(`serverInfo.version` is illustrative — the device reports its running firmware version.)

The client then sends the initialized notification — answered with `202` and no body:

```bash
curl -si http://tesla-key-esp32.local/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'
# HTTP/1.1 202 Accepted
```

(Statelessness means a bare `tools/call` without a prior `initialize` also works — useful
for smoke tests — but real clients always run the lifecycle.)

### 2. `tools/list`

```bash
curl -s http://tesla-key-esp32.local/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
```

```json
{"jsonrpc":"2.0","id":2,"result":{"tools":[
  {"name":"get_vehicle_state",
   "description":"Read cached vehicle state (VIN, link state, SOC, charging). Never wakes the car.",
   "inputSchema":{"type":"object","properties":{}}},
  {"name":"wake_up","description":"Wake the vehicle over BLE.",
   "inputSchema":{"type":"object","properties":{}}},
  ...
  {"name":"set_charging_amps","description":"Set the charging current in amps.",
   "inputSchema":{"type":"object",
     "properties":{"amps":{"type":"integer","minimum":0,"maximum":48}},
     "required":["amps"]}},
  ...]}}
```

### 3. `tools/call` — read state (never wakes the car)

```bash
curl -s http://tesla-key-esp32.local/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
       "params":{"name":"get_vehicle_state","arguments":{}}}'
```

```json
{"jsonrpc":"2.0","id":3,"result":{
  "content":[{"type":"text","text":
    "{\"vin\":\"<VIN>\",\"paired\":true,\"session_available\":false,\"link\":\"asleep\",\"last_seen_s\":4210,\"soc\":72,\"usable_soc\":70,\"charging_state\":\"Disconnected\",\"charge_limit\":80,\"charge_amps\":16,\"charger_power_kw\":0}"}],
  "isError":false}}
```

### 4. `tools/call` — command with arguments

```bash
curl -s http://tesla-key-esp32.local/mcp \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call",
       "params":{"name":"set_charging_amps","arguments":{"amps":16}}}'
```

Success and failure use the same shape — check `isError`:

```json
{"jsonrpc":"2.0","id":4,"result":{
  "content":[{"type":"text","text":"command executed successfully"}],"isError":false}}
```

```json
{"jsonrpc":"2.0","id":4,"result":{
  "content":[{"type":"text","text":"vehicle not reachable"}],"isError":true}}
```

### 5. Error cases

```bash
# Unknown tool → JSON-RPC error -32602
curl -s http://tesla-key-esp32.local/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"door_unlock"}}'
# {"jsonrpc":"2.0","id":5,"error":{"code":-32602,"message":"unknown tool"}}

# Unimplemented capability → -32601
curl -s http://tesla-key-esp32.local/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":6,"method":"resources/list"}'
# {"jsonrpc":"2.0","id":6,"error":{"code":-32601,"message":"method not found"}}

# Malformed body, including invalid raw UTF-8 in a JSON string → -32700
curl -s http://tesla-key-esp32.local/mcp -H 'Content-Type: application/json' -d 'not json'
# {"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"parse error"}}

# Batch → -32600 (single messages only)
curl -s http://tesla-key-esp32.local/mcp -H 'Content-Type: application/json' -d '[]'
# {"jsonrpc":"2.0","id":null,"error":{"code":-32600,"message":"batching not supported"}}

# GET → 405 (no SSE stream offered)
curl -si http://tesla-key-esp32.local/mcp | head -1
# HTTP/1.1 405 Method Not Allowed
```

### JSON-RPC error codes

| Code | Meaning here |
|------|--------------|
| `-32700` | Body is not valid JSON (including malformed raw UTF-8 or invalid string escapes), empty, or could not be received. |
| `-32600` | Invalid request: body over the 2 KB cap (HTTP `413`, message `request body too large`); nesting beyond 16 arrays/objects (`JSON nesting too deep`); more than 64 values (`too many JSON values`, `id:null`); escaped U+0000 anywhere (`JSON NUL escape not supported`); a batch array or non-object body; `jsonrpc` missing or not the exact string `"2.0"` (a separate valid `id` is still correlated); duplicate object keys at any depth (a duplicate `id` is ambiguous, so `id:null`); no `method`; an invalid `id`; or a `notifications/*` message that carries an `id`. |
| `-32601` | Method not implemented (`resources/*`, `prompts/*`, …). |
| `-32602` | `tools/call` with an unknown tool, an absent required argument (`missing required argument: <key>`) or a present-but-unparseable one (`integer required: <key>`, `boolean required: <key>` or `invalid argument: <key>`, according to the registry type and input). |
| `-32603` | HTTP `503`: receive-buffer, cJSON materialization or pre-effect response-build/print allocation failure. |

These codes describe handler replies. Before dispatch, runtime admission can return the flat
`503` JSON body documented in [API.md](API.md#response-classes); a contained pre-effect C++ exception
returns `503 text/plain`, not `-32603`. A pre-effect response-build/print OOM envelope uses `id:null`.

After a command attempt, reply-build/print OOM or an exception uses a correlated HTTP `200` result
with the original validated ID, one text content block, `reply_degraded:true` and the application
`outcome` from [API.md](API.md#response-classes). Known API success has `isError:false`; uncertainty
or incomplete identity recovery has `isError:true`. This is not a pre-execution `-32603` refusal.
A failed header/partial send ends the attempt without a second reply or command. A lost or uncertain
acknowledgement does not establish that a command failed; verify state before another attempt.

---

## Client integration

Configure a local MCP client for **Streamable HTTP** at `http://tesla-key-esp32.local/mcp` or the
board IP. Initialize, list tools, then call `get_vehicle_state` to confirm cached read access.
Commands such as `set_charge_limit` require deliberate action and the car may refuse them.

A client must accept JSON-only responses and plain HTTP on your trusted LAN. There is no SSE stream
or session ID. Use at least a 30 s command timeout. For stdio-only clients, choose a bridge that
supports this transport and evaluate its local executable/network trust separately. Client-specific
configuration belongs to that client's documentation; do not expose the board publicly to reach it.

## Operational notes & limits

- **One request at a time.** The httpd serves requests sequentially and commands hold the
  handler for the BLE round-trip; concurrent MCP calls (or an MCP call racing an evcc
  command) simply queue. Keep client timeouts ≥ 30 s.
- **2 KiB body cap.** The normal API server's per-request bound applies to `POST /mcp`; real MCP
  requests are far smaller, and an oversized body gets HTTP `413` with `-32600`.
  Requests also allow at most 64 JSON values, counting root, containers and scalars but not object
  names. Excess values are rejected before cJSON allocation with HTTP 200, `-32600`, `id:null`.
  This limit preserves existing syntax/depth/NUL error priority.
- **Don't poll commands — poll state.** `get_vehicle_state` is free (cache-only, no BLE);
  `wake_up`/commands cost a BLE connect and, repeated needlessly, keep the car from
  sleeping. A well-behaved agent checks `link` first and only wakes when it must.
- **The car is the authority.** The device validates arguments and forwards; the vehicle may
  still refuse (already at limit, cable locked, …) — that reason comes back verbatim in
  the `isError` result text.
- **No push.** The server never initiates messages (no SSE): to observe a change, poll
  `get_vehicle_state` — or use the read-only [MQTT/Home-Assistant bridge](ARCHITECTURE.md#home-assistant-mqtt-bridge)
  for event-style updates.

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| Client reports "connection refused / timeout" | Wrong IP, or mDNS not resolving — use the device IP from your router; device and client must be on the same LAN. |
| Client complains about SSE / `405` on GET | Expected — this server is stateless JSON-only. Configure the client for plain Streamable HTTP (`--transport http-only` for `mcp-remote`). |
| `tools/call` returns `isError: true`, `"vehicle not reachable"` | Car out of BLE range or link cold — check range and pairing; an intentional wake may be needed before retrying. |
| `tools/call` returns `isError: true` with the car's refusal text | The vehicle rejected the request; inspect its reason before retrying. |
| `get_vehicle_state` has only `vin`/`paired`/`session_available`/`link` | Fresh boot with the car asleep: telemetry caches are empty until the first contact. |
| Commands fail although the car is nearby | Check BLE availability and the hero's pairing status. `paired` reports a stored VCSEC session, not fresh vehicle acceptance. Reconnection and same-key enrollment run automatically; approve any enrollment prompt with the NFC card on the reader. Do not regenerate the key from an error hint. |
| HTTPS / auth errors from a strict client | The endpoint is plain HTTP without credentials by design — see [`SECURITY.md`](SECURITY.md#http-api-exposure). Use a client/bridge that permits HTTP on a trusted LAN. |
