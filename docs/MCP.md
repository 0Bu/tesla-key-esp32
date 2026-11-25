# MCP integration guide

Configure **Streamable HTTP** at `http://<ESP32-IP>/mcp`. Keep the unauthenticated HTTP API on
a trusted LAN. Use the board's IP when multiple boards share `tesla-key-esp32.local`.
[HTTP exposure](SECURITY.md#http-api-exposure)

## Endpoint & transport

| Property | Contract |
|---|---|
| Request | `POST /mcp`, `Content-Type: application/json`, one JSON-RPC 2.0 object, max 2 KiB |
| Transport | Stateless, JSON replies, no SSE or session ID. `GET` returns `405` with `Allow: POST`. The `MCP-Protocol-Version` header is ignored. |
| Revisions | `2025-06-18` and `2025-03-26`. An unsupported revision negotiates `2025-06-18`. |
| Envelope | `jsonrpc` must be exactly `"2.0"`. A valid message with a string method and no ID returns `202` with an empty body and no dispatch, including id-less `tools/call`. |
| ID | String up to 64 bytes or canonical decimal JSON integer from `-9007199254740991` to `9007199254740991`. Echoes exact decimal. Fractions, exponents, negative zero, null and other types are invalid. |
| JSON bounds | Depth ≤16 and ≤64 values, counting root, containers and scalars. Batches, decoded duplicate keys and escaped U+0000 are invalid. |

### Supported methods

| Method | Result |
|---|---|
| `initialize` | Negotiated revision, `tools` capability, server name/version and instructions |
| `notifications/initialized` | HTTP `202`, empty body |
| `ping` | `{}` |
| `tools/list` | Tool definitions and argument schemas |
| `tools/call` | `content` and `isError` |

Initialization advertises the [signed HTTP OTA upload](API.md#direct-signed-firmware-upload).
The upload needs explicit authority for the target board, OTA and reboot.

## Tools

The enrolled **Charging Manager** role covers charging, wake and reads. Acceptance of a specific
action depends on the vehicle, including [charge-port commands](adr/0005-tesla-ble-seam.md#native-execution-framing-and-session-reuse).

| Tool | Arguments | Result or action |
|---|---|---|
| `get_vehicle_state` | — | Cached JSON text, without scanning, connecting or waking |
| `wake_up` | — | Wake with fresh Infotainment confirmation |
| `charge_start` / `charge_stop` | — | Start or stop charging |
| `charge_port_open` / `charge_port_close` | — | Open or close the charge port |
| `set_charging_amps` | `amps`, required integer 0–48 | Set and verify current |
| `set_charge_limit` | `percent`, required integer 50–100 | Set the charge limit |
| `set_scheduled_charging` | `enable`, required boolean. `start_minutes`, integer 0–1439 | Schedule in minutes after local midnight |

Integer arguments accept integral JSON numbers and canonical decimal strings. Booleans also
accept numeric `0`/`1`. Missing required, malformed or out-of-range arguments return `-32602`.
[Canonical integer syntax](API.md#commands)

Command failures return `isError:true` with reason text. Successful no-ops return `isError:false`
and `structuredContent.noop` (`already_set` on any command; `complete`, `is_charging` or
`not_charging` on the matching `charge_start`/`charge_stop` pair). Check vehicle state before
repeating an uncertain command.
[Shared command policy](API.md#commands)

`get_vehicle_state` reports `vin`, `paired` (stored VCSEC session), `session_available` (usable
VCSEC session in RAM), `link` and `charge_available`. Session flags do not prove reachability or
fresh acceptance. `last_seen_s` measures time since live contact when known. Available charge data
can include `soc`, `usable_soc`, `charging_state`, `charge_limit`, `charge_amps` and
`charger_power_kw` when reported by the vehicle. See [link states](ARCHITECTURE.md#sleep--link-state-the-single-source-of-truth)
and the [shared charge freshness policy](API.md#vehicle-data).

## Wire examples (curl)

Set the target URL before running the read-only lifecycle example.

```bash
mcp_url='http://<ESP32-IP>/mcp'
mcp_post() { curl -sS "$mcp_url" -H 'Content-Type: application/json' -d "$1"; }

mcp_post '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{
  "protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"0"}}}'
mcp_post '{"jsonrpc":"2.0","method":"notifications/initialized"}'
mcp_post '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
mcp_post '{"jsonrpc":"2.0","id":3,"method":"tools/call",
  "params":{"name":"get_vehicle_state","arguments":{}}}'
```

A command uses the same envelope. This request changes charging current.

```json
{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"set_charging_amps","arguments":{"amps":16}}}
```

### JSON-RPC error codes

| Code | Cause |
|---|---|
| `-32700` | Empty, unreadable or malformed JSON, including invalid raw UTF-8 and string escapes |
| `-32600` | Invalid request or exceeded JSON bounds, including absent method or a `notifications/*` message with an ID. Oversize returns HTTP `413`. Too many values returns HTTP `200` and `id:null`. |
| `-32601` | Unknown method |
| `-32602` | Unknown tool or invalid/missing argument |
| `-32603` | Pre-effect allocation failure, HTTP `503` |

An invalid or duplicate ID correlates as `id:null`. A separate unique valid ID is retained
where the parser can correlate it. Pre-effect reply allocation errors use `id:null`.
The shared admission/exception fallback can return flat `503` JSON or plain text.

A post-command degraded reply uses HTTP `200`, the original ID, one text block,
`reply_degraded:true` and an `outcome`. Known success has `isError:false`. Uncertain execution or
incomplete identity recovery has `isError:true`. Partial sends end the reply.
[HTTP reply classes](API.md#response-classes)

## Client integration

Accept JSON-only replies. A stdio-only client needs a Streamable HTTP bridge. Allow at least
30 s for the 20 s command budget plus queueing behind the single HTTP worker. Poll
`get_vehicle_state` or subscribe to [MQTT](API.md#mqtt-topics). Repeated wake or control commands
can keep the car awake.

## Troubleshooting

| Symptom | Check |
|---|---|
| Connection or VIN mismatch | Target IP, shared mDNS name, plain HTTP and trusted LAN |
| SSE required by client | Streamable HTTP mode (for `mcp-remote`, `--transport http-only`) |
| Missing charge fields after boot | A sleeping car can leave caches empty until live contact. |
| Refusal or unreachable link | Check the reason, BLE range and [pairing status](USER_GUIDE.md#web-status-and-pairing-recovery). |
