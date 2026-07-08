# MCP integration guide

The device is an MCP ([Model Context Protocol](https://modelcontextprotocol.io/)) server:
any MCP-capable client — Claude Desktop, Claude Code, VS Code, or an agent you build with
the MCP SDKs — can read the vehicle's cached state and drive the charging command set
directly, with no proxy process in between. The endpoint runs on the same `esp_http_server`
as the REST API and web UI, on port 80.

This is the user/integrator guide: wire format, tool reference, client configuration and
troubleshooting. For the firmware-internal design (heap budget, code layout, why stateless)
see [`ARCHITECTURE.md`](ARCHITECTURE.md#mcp-endpoint-mcp); for the security posture see
[`SECURITY.md`](SECURITY.md#http-api-exposure).

> **Trusted LAN only.** By default `/mcp`, like every other endpoint on this device, has no
> authentication and no TLS — anyone on the network can call it. The enrolled key is
> Charging Manager only (charging + wake; it cannot unlock or drive the car), and the
> endpoint grants nothing the open REST API doesn't already expose. Never port-forward it.
> Unlike the REST API, `/mcp` **can** optionally require a bearer token (MCP clients can
> send headers; evcc can't) — see [Authentication](#authentication).

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
| Response | Always `application/json` — the server never opens an SSE stream |
| `GET /mcp` | `405 Method Not Allowed` (+ `Allow: POST`) — no server-initiated stream is offered |
| Sessions | None. No `Mcp-Session-Id` header is issued or expected; every request is self-contained |
| `MCP-Protocol-Version` header | Ignored (nothing version-dependent happens after `initialize`) |
| Notifications (`notifications/*` — any message with a `method` but no `id`) | `202 Accepted`, empty body. A message with neither `method` nor `id` (`{}`) is malformed, not a notification → `-32600` |
| JSON-RPC batches (arrays) | Rejected with `-32600` (batching was removed in protocol `2025-06-18`) |
| Protocol revisions | `2025-06-18` and `2025-03-26`. Requesting anything else returns the server's latest (`2025-06-18`); the client may then disconnect per the spec |

Clients typically send `Accept: application/json, text/event-stream` — that is fine; the
spec lets the server pick either representation and this one always answers plain JSON.

### Authentication

**Off by default** — with no token configured, `/mcp` behaves exactly as before (open on
the trusted LAN). When a token is configured, every `POST /mcp` must carry

```
Authorization: Bearer <token>
```

or it is answered `401 Unauthorized` (+ `WWW-Authenticate: Bearer`) before any JSON-RPC
processing. `GET /mcp` (the `405`) stays open — it reveals nothing. The check is scoped to
`/mcp` only: the REST API stays open because evcc cannot send credentials
([`SECURITY.md`](SECURITY.md#http-api-exposure)); MCP clients all can (see the client
config examples below — add an `Authorization` header entry). The token comparison is
timing-safe, and its logic is host-tested (`tk::mcp_token_ok`, `main/logic/mcp.hpp`).

Configuring the token (max 128 chars, applied at boot):

- **Build time:** `idf.py menuconfig` → *Tesla Key Configuration* → *MCP bearer token*
  (`CONFIG_TESLA_MCP_TOKEN`).
- **Runtime override:** NVS key `mcp_token` in the `tesla_cfg` namespace. Whenever the key
  exists it wins over the Kconfig value — an existing **empty** NVS value deliberately
  disables auth without a reflash. (No HTTP endpoint sets this on purpose: an open
  endpoint that installs the token could also replace it.)

Remember the transport is still plain HTTP: the token rides in cleartext on the LAN. It
raises the bar from "anyone who can reach port 80" to "anyone who can sniff or knows the
token" — a scoping control, not a substitute for network segmentation.

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
| `wake_up` | — | Wakes the vehicle over BLE. |
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
  `start_minutes: "08:00"`) is likewise `-32602` (`invalid argument: <key>`), never
  silently defaulted. Loose-but-unambiguous encodings are accepted: numeric strings for
  integers (`"16"` → 16) and `0`/`1` for booleans. **Out-of-range integers are clamped**
  server-side to the bounds above (the schemas advertise the same bounds — both are
  generated from one spec table, so they cannot drift).
- **Failures are tool results, not protocol errors:** a refused or undeliverable command
  comes back as a normal `tools/call` result with `isError: true` and the reason as text —
  the real Tesla refusal (e.g. `complete` when the battery is already at its limit) when
  the car answered, or `vehicle not reachable` when it didn't. JSON-RPC errors are
  reserved for malformed requests (unknown tool/method, missing required args, parse
  errors).
- **`get_vehicle_state` is cache-fed.** Fields appear only when known: after a reboot
  with the car asleep, expect just `vin`, `paired` and `link`. `link` is the same value
  the web UI hero uses — one of `awake` / `asleep` / `idle` / `unreachable`, plus
  `unknown` before first contact (see
  [`ARCHITECTURE.md`](ARCHITECTURE.md#sleep--link-state-the-single-source-of-truth)) —
  and `last_seen_s` is seconds since the last live contact.

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
  "serverInfo":{"name":"tesla-key-esp32","version":"1.4.25"},
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
    "{\"vin\":\"5YJ3E1EA7KF000316\",\"paired\":true,\"link\":\"asleep\",\"last_seen_s\":4210,\"soc\":72,\"charging_state\":\"Disconnected\",\"charge_limit\":80,\"charge_amps\":16,\"charger_power_kw\":0}"}],
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

# Malformed / oversized (>2 KB) body → -32700
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
| `-32700` | Body is not valid JSON, empty, or over the 2 KB cap |
| `-32600` | Batch array or non-object body, a request whose `method` field is missing, or a `notifications/*` message that (wrongly) carries an `id` |
| `-32601` | Method not implemented (`resources/*`, `prompts/*`, …) |
| `-32602` | `tools/call` with an unknown tool name, an absent required argument (`missing required argument: <key>`), or a present-but-unparseable argument (`invalid argument: <key>`) |

---

## Client integration

### Claude Code

```bash
claude mcp add --transport http tesla-key http://tesla-key-esp32.local/mcp
```

Then, in a session: *"What's the state of my Tesla?"* → the model calls
`get_vehicle_state`; *"Set the charge limit to 90%"* → `set_charge_limit`.

### VS Code (GitHub Copilot agent mode)

`.vscode/mcp.json` in the workspace (or add via **MCP: Add Server**):

```json
{
  "servers": {
    "tesla-key": {
      "type": "http",
      "url": "http://tesla-key-esp32.local/mcp"
    }
  }
}
```

### Claude Desktop (and other stdio-only clients)

Claude Desktop launches local stdio servers, so bridge to the device with
[`mcp-remote`](https://www.npmjs.com/package/mcp-remote) (needs Node.js). In
`claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "tesla-key": {
      "command": "npx",
      "args": [
        "mcp-remote",
        "http://tesla-key-esp32.local/mcp",
        "--allow-http",
        "--transport", "http-only"
      ]
    }
  }
}
```

`--allow-http` is required because the device is plain HTTP (trusted LAN);
`--transport http-only` skips the SSE probing this server would answer with `405`.

### Python (official `mcp` SDK)

```python
# pip install mcp
import asyncio
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client

URL = "http://tesla-key-esp32.local/mcp"

async def main():
    async with streamablehttp_client(URL) as (read, write, _):
        async with ClientSession(read, write) as session:
            await session.initialize()

            tools = await session.list_tools()
            print([t.name for t in tools.tools])

            state = await session.call_tool("get_vehicle_state", {})
            print(state.content[0].text)          # JSON text blob

            result = await session.call_tool("set_charge_limit", {"percent": 90})
            print("failed:" if result.isError else "ok:", result.content[0].text)

asyncio.run(main())
```

### Claude API (MCP connector)

The Messages API can call the device directly via the MCP connector — but the API servers
sit outside your LAN, so this only works if the endpoint is reachable from them (e.g.
through a VPN/tunnel that you control — do **not** expose the device itself):

```python
client.beta.messages.create(
    model="claude-opus-4-8", max_tokens=1024,
    betas=["mcp-client-2025-11-20"],
    mcp_servers=[{"type": "url", "url": "https://<your-tunnel>/mcp", "name": "tesla-key"}],
    tools=[{"type": "mcp_toolset", "mcp_server_name": "tesla-key"}],
    messages=[{"role": "user", "content": "Is my car charging?"}],
)
```

For LAN-local automation prefer a local client (Claude Code, the Python SDK above, or an
agent process on the same network).

---

## Operational notes & limits

- **One request at a time.** The httpd serves requests sequentially and commands hold the
  handler for the BLE round-trip; concurrent MCP calls (or an MCP call racing an evcc
  command) simply queue. Keep client timeouts ≥ 30 s.
- **Body cap 2 KB.** Applies to every POST on the device. All real MCP requests are far
  smaller; an oversized body gets `-32700`.
- **Don't poll commands — poll state.** `get_vehicle_state` is free (cache-only, no BLE);
  `wake_up`/commands cost a BLE connect and, repeated needlessly, keep the car from
  sleeping. A well-behaved agent checks `link` first and only wakes when it must.
- **The car is the authority.** The device clamps arguments and forwards; the vehicle may
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
| `tools/call` returns `isError: true`, `"vehicle not reachable"` | Car out of BLE range or link cold — call `wake_up` first, retry after a few seconds. |
| `tools/call` returns the car's own refusal text (e.g. `complete`) | Not an error in the chain — the car declined (battery already at limit, etc.). |
| `get_vehicle_state` has only `vin`/`paired`/`link` | Fresh boot with the car asleep: caches are empty until the first contact. |
| Commands fail although the car is nearby | Check `paired` in `get_vehicle_state` / the web UI — a lost pairing (key deleted on the car) needs a re-pair. |
| HTTPS / auth errors from a strict client | The endpoint is plain HTTP without credentials by design — see [`SECURITY.md`](SECURITY.md#http-api-exposure). Use a client/bridge that permits HTTP on a trusted LAN. |
