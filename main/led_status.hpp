#pragma once

class VehicleController;

// ─── On-device status LED (LilyGo T-Dongle-C5 / T-Dongle-S3 underside APA102) ──────
// Drives the single addressable RGB pixel on the bottom of the dongle as a status
// indicator: WiFi/BLE search (breathing "wave"), pairing (pulse), charging (green
// swell), state-of-charge colour when parked, OTA in flight, and amber/red for
// warnings/errors. The colour+animation for every state is decided by the pure,
// host-tested ladder in logic/led_status.hpp; this file only samples state and pushes
// bytes to the LED.
//
// Design constraints (match the rest of the firmware):
//   • Reads ONLY cached state (link_state / ble_connected / has_session /
//     get_cached_charge / reauth_required / ble_connect_fail / ota_get_status /
//     wifi_is_connected) — it NEVER triggers a BLE round-trip, so it can't wake the
//     car or queue behind a poll in the single BLE FIFO. Same guarantee as the display.
//   • Independent of MQTT and of the display — needs neither, so it works on a
//     T-Dongle-S3 (no panel) as well as the C5.
//   • Tiny + coex-safe: 1 pixel, an ~8-byte bit-banged APA102 frame, no heap
//     allocation, low update rate — no pressure on the contiguous-heap budget or on
//     WiFi/BLE coexistence (WIFI_PS_MIN_MODEM stays untouched).
//
// Compiled only when CONFIG_TESLA_LED_ENABLED (a no-op stub otherwise), so one source
// tree still serves every board. The APA102 data/clock GPIOs and brightness come from
// Kconfig — defaults are the T-Dongle-C5 wiring (DI=5, CI=4); a T-Dongle-S3 sets 40/39.
void led_status_start(VehicleController& vehicle);
