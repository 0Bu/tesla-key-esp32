# Claude Design brief — web UI redesign (four variants)

This is a self-contained brief for **Claude Design**. It asks for **four distinct, modern and
functional design variants** of the on-device web UI served by `tesla-key-esp32`. Paste the
[prompt](#prompt-to-paste-into-claude-design) section into Claude Design; the remaining sections
are the reference material it (and the reviewer) should use.

The brief is a design exploration only. It does not change firmware behavior, the `/status`
contract, command semantics or any endpoint. Porting a chosen variant into `main/www/` is a
separate implementation task that must keep every constraint below.

## Product context

`tesla-key-esp32` is a small ESP32 board that acts as a Bluetooth (BLE) key for a Tesla. It sits
near the parking spot and exposes a local HTTP API used by evcc, Home Assistant (MQTT) and an MCP
client. The web UI is the owner's control and health page for that device, opened on a phone,
tablet or desktop browser inside the home network.

Typical visits are short:

1. **Glance** — Is the car reachable? Battery level? Is it charging, and at what power?
2. **Act** — Start/stop charging, wake the car.
3. **Fix** — Pair the key, set the VIN, set the MQTT broker or Syslog server, see why the
   Bluetooth link fails, install a firmware update.

## Hard technical constraints

Every variant must be implementable within these limits. Designs that need anything else are out
of scope.

- **Single self-contained page.** `index.html`, `style.css` and `app.js` are inlined at build
  time into one document, gzipped and embedded in firmware flash. No external fonts, CDNs, icon
  fonts, images or network requests other than the device's own endpoints. The device may have
  no internet access.
- **System font stack only** (`system-ui, -apple-system, "Segoe UI", Roboto, sans-serif`, and
  `ui-monospace, SFMono-Regular, Menlo, Consolas, monospace` for VIN, fingerprint and versions).
- **Icons as small inline SVG** (stroke or fill, `currentColor`). No emoji as UI icons.
- **Vanilla HTML/CSS/JS.** No framework, no build step beyond inlining, no web components
  library. CSS custom properties are fine; `color-mix()` is already in use.
- **Size budget.** The current page is about 95 KB uncompressed (CSS about 21 KB). A variant's
  CSS should stay at or below about 24 KB uncompressed; prefer less. No raster assets.
- **Cheap rendering.** The page polls `GET /status` every 4 s and ticks a countdown every
  1 s. Avoid heavy filters, large `backdrop-filter` areas, continuous full-page animation or
  layout thrash. Animations must be CSS-only, subtle and disabled under
  `prefers-reduced-motion: reduce`.
- **Light and dark** via `prefers-color-scheme`, both first-class.
- **Responsive**: 320 px phones up to desktop. Mobile first; a tablet/desktop layout must use the
  extra width sensibly rather than just scaling up. Respect `env(safe-area-inset-*)`.
- **Accessibility**: WCAG 2.2 AA contrast, visible `:focus-visible` rings, touch targets at least
  44 × 44 px, real `<button>` elements, `aria-live` for status and toasts, color is never the
  only signal (pair every status color with a text or icon).
- **English UI copy.** Keep the existing wording unless a clearer, equally short phrase exists.

## Content and state inventory

All variants must present exactly this content. Nothing may be dropped; placement, grouping and
emphasis are free.

### Header

- Product name `tesla-key-esp32` and a brand mark (the current mark is a lightning bolt on a
  red `#e82127` rounded square; it may be refined, it must stay recognizable).
- Meta line: device IP / hostname, firmware version (tappable: "check for updates"),
  update-channel menu (**Release** / **Development**), inline OTA progress (ring + text: checking,
  downloading n %, rebooting, error).

### Banners (top of content, zero or one at a time)

- **Re-pair needed** (warning): the key was rejected by the car.
- **Safe Mode active** (warning): after repeated crashes; Bluetooth, commands and telemetry are
  stopped, only diagnostics and firmware update remain.

### Vehicle hero (the primary card)

A circular battery gauge (0–100 %, color graded red → amber → green) that is also the primary
action button. States and their exact messages:

| State | Headline | Sub text | Gauge / icon | Primary action |
| --- | --- | --- | --- | --- |
| Awake, not charging | car status, e.g. `Idle` | — | static SOC ring | tap: start charging |
| Charging | `Charging` | — | animated SOC ring | tap: stop charging |
| Charge complete | `Charge complete` | — | full static ring | disabled |
| Asleep (proven) | `Vehicle asleep` | `Tap the icon to wake the car.` | grey pulsing ring + moon | tap: wake |
| Parked (not proven asleep) | `Parked` | `No live reading — tap the icon to wake the car.` | grey ring + parked car | tap: wake |
| Waking | `Waking up…` | `Reaching your Tesla over Bluetooth…` | faster pulse + trembling alarm clock | disabled |
| Unreachable | `Vehicle unreachable` | `Bring the device within Bluetooth range.` | empty ring + bolt | none |
| Checking | `Checking status…` | `Bluetooth connected — checking status…` | neutral | none |
| BLE connect failing | `Connection failed` | `The car has too many Bluetooth devices connected.` or `Move the device closer, or disconnect other devices using the car.` | empty ring + Bluetooth glyph | none |
| Not configured | `Set up needed` | `Add the vehicle VIN below to begin.` / `Generate a security key below.` | neutral | points to VIN / key |
| Pairing | `Pairing` | `Approve the request on your Tesla’s touchscreen.` | neutral | none |
| Searching | `Looking for your car` | `Bring the device within Bluetooth range.` | neutral | none |

Stat chips under the headline (0–3, equal width):

- Charging: **Power** `11 kW`, **Current** `16 A` (green).
- Asleep / parked / unreachable: last-known **Battery** `%` and **Idle** time (`<1 min`, `5 min`,
  `2 h`, `3 d`).
- Awake and not charging, when active: **Overheat** (cabin overheat protection AC draw, amber,
  e.g. `1.4 kW`) and **Defrost** (AC draw, blue).

A busy state (command in flight) must be visible on the gauge without layout shift.

### Setup tiles

- **Vehicle**: VIN (monospace, 17 chars) plus a sub line; editable (currently a native prompt —
  the redesign should show an inline field or sheet). Changing an existing VIN needs a clear
  destructive confirmation: "generates a new security key and clears the stored pairing".
- **Security key**: key fingerprint (monospace) plus pairing state sub line; action "Generate /
  regenerate key" with destructive confirmation ("the current key is invalidated and you must
  re-pair").

### Connections list

One row each, with a status value and, where applicable, an edit affordance:

- **Wi-Fi** (SSID + 4-bar signal glyph + dBm) or **Ethernet** (link speed) — read-only.
- **BLE**: Connected (signal bars) / Connecting with a live countdown (`gives up in 12 s`) /
  Waiting with countdown (`retries in 30 s`) / Disconnected (outlined empty bars) / Unknown
  (amber "searching" animation).
- **MQTT**: broker `host:port` + connected / disconnected / disabled; editable (hint: saved
  credentials stay hidden; leaving the host unchanged keeps them).
- **Syslog**: `host:port` or disabled; editable (empty disables it).

### Firmware update dialog (modal)

Title "Firmware update", version line `1.8.0 → 1.9.0`, channel badge (Release / Development /
PR preview), a bulleted changelog (or "No changelog was supplied for this update."), help text
"The device downloads the firmware and reboots when done.", buttons **Cancel** and
**Install update**.

### Toasts

Short feedback, three tones: `info`, `ok`, `err` (error toasts stay longer). Examples:
`Saving VIN…`, `VIN saved · rebooting`, `Invalid VIN — must be 17 characters`.

### Setup portal (secondary screen, `setup.html`)

Shown by the device's own access point before it has network credentials. One centered card:
title "Wi-Fi setup", fields **Wi-Fi network (SSID)**, **Wi-Fi password**, optional **Tesla VIN**,
a save button and a short explanation. Each variant should include its version of this screen.

## Sample data for mockups

Use only these placeholder values (documentation ranges, no real identifiers):

- Device: `tesla-key.local` · `192.0.2.10` · firmware `1.9.0` · channel Release.
- VIN: `5YJ3E1EA0KF000000`; key fingerprint: `a1b2 c3d4 e5f6 0718`.
- Wi-Fi: `HomeNetwork`, −58 dBm, 3 of 4 bars. Ethernet alternative: `100 Mbit/s`.
- MQTT: `192.0.2.20:1883`, connected. Syslog: `192.0.2.30:514`.
- Charging: SOC 64 %, limit 80 %, 11 kW, 16 A. Asleep: last SOC 72 %, idle 3 h.
- OTA changelog: "Faster BLE reconnect", "Defrost power chip", "Safer MQTT reconfiguration".

## The four variants

Each variant must be a genuinely different design direction, not a recolor. Keep the content
inventory identical across all four so they can be compared side by side.

### Variant A — "Instrument"

Inspired by a vehicle instrument cluster. Dark-first with a refined light mode, deep neutral
surfaces, one red accent. The battery gauge is large and centered, with thin precise strokes,
tabular numerals and a clear charge-limit tick. Secondary information sits in a quiet, compact
band below. Motion is restrained and purposeful (charging flow along the ring). Feels premium,
calm and automotive.

### Variant B — "Control Center"

A bento/widget grid in the style of modern smart-home dashboards. Rounded tiles of varying size:
a large vehicle tile, medium tiles for charging stats, key and VIN, small tiles for each
connection. On phones it is a clean two-column grid; on desktop it expands to a three- or
four-column board. Each tile has one clear primary action. Friendly, tactile, highly scannable.

### Variant C — "Console"

Data-forward and dense for power users who also run evcc and Home Assistant. Compact
list/table layout, strong typographic hierarchy, monospace for identifiers and values, status
dots with labels, visible timestamps ("updated 4 s ago"), inline edit fields instead of dialogs.
Desktop uses a sidebar or two-pane layout (vehicle + connections | device + firmware). Looks
like a well-designed developer tool, not a terminal cliché.

### Variant D — "Glance"

Radically focused: one answer per screen. The whole top of the page is a single status statement
("Charging · 64 %", "Asleep · 72 %") with one large primary action. Everything else — VIN, key,
connections, firmware — is collapsed into clearly labeled, expandable sections or a bottom sheet
("Device & connections"). Large type, generous spacing, high contrast, excellent for one-handed
phone use and for users with low vision.

## Deliverables

For **each** variant:

1. Phone (390 px) and desktop (1280 px) views, light and dark.
2. Hero states: charging, asleep, unreachable, BLE connection failed, set up needed, pairing,
   Safe Mode banner.
3. Connections list with BLE in connected, countdown and disconnected states.
4. The firmware update dialog and an in-progress OTA indicator.
5. VIN edit and key regeneration flows including the destructive confirmation.
6. Toast examples (info, ok, err).
7. The setup portal screen.
8. Design tokens: color (light/dark), type scale, spacing, radii, elevation, motion durations —
   named as CSS custom properties so they can replace the `:root` block in
   `main/www/style.css`.
9. A short rationale (3–5 sentences): who the variant is for and its trade-offs.

Finish with a **comparison table** across the four variants: glanceability, density,
one-handed use, accessibility, estimated CSS size and implementation effort against the current
markup.

## Acceptance criteria

- All states and content from the inventory are present in every variant.
- Every mockup is buildable with plain HTML, CSS and inline SVG under the size budget.
- Contrast meets WCAG 2.2 AA in both themes; no information is carried by color alone.
- No layout shift when values update every 4 s or when a command is in flight.
- Destructive actions (VIN change, key regeneration) are visually distinct and require
  confirmation.
- No real personal data appears in any mockup.

## Prompt to paste into Claude Design

```text
Design a modern, functional web UI for "tesla-key-esp32", a small ESP32 device that acts as a
Bluetooth key for a Tesla and serves a local status/control page on the home network. Create
FOUR clearly different design variants of the same page, using identical content in each, so
they can be compared side by side:

A "Instrument" — automotive instrument-cluster feel, dark-first, one red accent (#e82127),
   large precise battery gauge, calm restrained motion.
B "Control Center" — bento/widget grid like a modern smart-home dashboard, tiles of varying
   size, 2 columns on phone, 3-4 on desktop, one clear action per tile.
C "Console" — dense, data-forward power-user layout, lists and inline edit fields, monospace
   identifiers, status dots with labels, "updated 4 s ago" timestamps, two-pane desktop.
D "Glance" — one big status statement and one primary action at the top; VIN, key,
   connections and firmware collapsed into expandable sections or a bottom sheet; large type,
   high contrast, one-handed phone use.

Page content (all required in every variant):
- Header: product name + lightning-bolt brand mark, IP/hostname, firmware version (tap to check
  for updates), update channel menu (Release / Development), inline OTA progress.
- Optional warning banner: "Re-pair needed" or "Safe Mode active".
- Vehicle hero: circular battery gauge (0-100 %, red->amber->green) that doubles as the main
  button (start/stop charging, or wake the car). States: Idle, Charging, Charge complete,
  Vehicle asleep, Parked, Waking up…, Vehicle unreachable, Checking status…, Connection failed,
  Set up needed, Pairing, Looking for your car. Stat chips: Power kW / Current A while
  charging; last Battery % and Idle time while asleep; Overheat and Defrost kW when active.
- Setup: Vehicle VIN (editable) and Security key fingerprint (regenerate), both with a clear
  destructive confirmation.
- Connections: Wi-Fi (SSID, 4-bar signal, dBm) or Ethernet (link speed); BLE (connected /
  connecting with countdown "gives up in 12 s" / waiting "retries in 30 s" / disconnected);
  MQTT broker (editable); Syslog server (editable).
- Firmware update modal: version "1.8.0 -> 1.9.0", channel badge, changelog list, Cancel /
  Install update.
- Toasts: info, ok, err.
- Secondary screen: Wi-Fi setup portal card (SSID, password, optional VIN, Save).

Constraints: one self-contained HTML page with inline CSS/JS, system fonts only, inline SVG
icons, no external assets or libraries, CSS at most ~24 KB, light and dark mode via
prefers-color-scheme, responsive from 320 px to desktop, WCAG 2.2 AA contrast, 44 px touch
targets, visible focus rings, never color-only status, subtle CSS-only motion that respects
prefers-reduced-motion, no layout shift when data refreshes every 4 s.

Sample data: tesla-key.local, 192.0.2.10, firmware 1.9.0, VIN 5YJ3E1EA0KF000000, key
fingerprint a1b2 c3d4 e5f6 0718, Wi-Fi "HomeNetwork" -58 dBm, MQTT 192.0.2.20:1883, Syslog
192.0.2.30:514, charging 64 % (limit 80 %) at 11 kW / 16 A, asleep 72 % idle 3 h.

For each variant deliver: phone (390 px) and desktop (1280 px) in light and dark; the key hero
states (charging, asleep, unreachable, connection failed, set up needed, pairing, Safe Mode);
BLE row states; the update modal and OTA progress; VIN edit and key regeneration flows; toasts;
the setup portal; design tokens as CSS custom properties; and a 3-5 sentence rationale. End
with a comparison table (glanceability, density, one-handed use, accessibility, CSS size,
implementation effort).
```
