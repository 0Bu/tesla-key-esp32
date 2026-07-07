#pragma once

class VehicleController;

// ─── On-device status display (LilyGo T-Dongle-C5, 0.96" ST7735 80x160) ───────
// Renders the same charge/connection values as the web UI directly on the panel:
// SOC ring, charging state, power/current, and the WiFi / BLE connection lines.
// Mirrors the web-UI layout (see tools/display_sim.py for the offline validation
// render).
//
// Design constraints (match the rest of the firmware):
//   • Reads ONLY cached state via VehicleController::get_cached_charge() and the
//     ble_*/link_state accessors — it NEVER triggers a BLE round-trip, so it can
//     never wake the car or queue behind a poll in the single BLE FIFO.
//   • MQTT is intentionally NOT shown — the panel is a car/link status display, not
//     a broker monitor — so it carries no MQTT dependency at all.
//   • Framebuffer lives in PSRAM (the T-Dongle-C5's 8 MB) when present, falling back
//     to internal RAM otherwise; only a one-line bounce buffer is internal/DMA.
//
// No-op unless CONFIG_TESLA_DISPLAY_ENABLED — so the default ESP32-S3 build (no
// panel) is completely unaffected.
void display_start(VehicleController& vehicle);
