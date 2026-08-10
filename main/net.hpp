#pragma once

#include "esp_netif.h"
#include "logic/net_link.hpp"

// The firmware's ONE network-transport seam. Everything above it — the HTTP server, MQTT,
// syslog, mDNS, SNTP, OTA, the display and the LED — asks these questions and never touches
// esp_wifi (or, from the Ethernet backend, esp_eth) directly.
//
// Before this existed the answers were WiFi-shaped: a hand-declared `wifi_is_connected()`
// repeated as an `extern` in five translation units, and `esp_netif_get_handle_from_ifkey(
// "WIFI_STA_DEF")` hardcoded in three more (the watchdog's gateway probe, /status.ip and the
// Home Assistant config URL). Each was correct while WiFi was the only transport and silently
// wrong the moment it was not.
//
// Threading: every getter here is a relaxed read of an atomic written by the event-loop task.
// They are safe to call from any task at any time, including before net_init() — which is
// what lets the display and LED tasks start before the link does and simply render "searching".
namespace tk {

// One-time bring-up of the shared substrate every transport needs: esp_netif and the default
// event loop. Idempotent — the second call is a no-op — because provisioning_run() may have
// created both already on the setup-AP path. Call before any net_start_*().
void net_init();

// Is a W5500 Ethernet controller actually wired to this board? Probes the SPI bus ONCE and
// caches the answer; safe to call very early, and deliberately CHEAP — it reads one identity
// register and does not wait for a link or a lease.
//
// Call it before deciding whether a device with no stored WiFi credentials should fall into the
// setup portal: a wired board does not need credentials, and stranding it in a captive AP it
// has no reason to run would be a regression created by adding a transport.
//
// Always false on a build without the Ethernet backend, and on the T-Dongle-S3 (whose ST7735
// panel clock is the same GPIO the probe would drive as SPI SCLK).
bool net_eth_probe();

// Bring up the Ethernet interface and wait up to CONFIG_TESLA_ETH_WAIT_S for a DHCP lease.
// Returns true only with a lease in hand — a controller with no cable, a dead switch port or an
// absent DHCP server all return false so the caller can give WiFi a turn. The driver keeps
// running either way, so a cable plugged in later still comes up.
bool net_start_eth();

// Bring up the WiFi station and wait for a DHCP lease.
//
// rollback_pending: a /set_wifi change is on trial this boot, so a failure is not simply
// "fall back to the setup portal" — it may mean restoring the previous credentials. That
// changes how long we wait and why (logic/wifi_rollback.hpp owns the policy), hence a
// parameter rather than a second function.
//
// Returns true once an IP arrived; false when the boot window (or the reason-aware rollback
// deadline) is spent.
bool net_start_wifi(const char* ssid, const char* password, bool rollback_pending);

// True while SOME transport holds an IP. The successor to wifi_is_connected(): same meaning,
// no assumption about which radio (or none) is behind it.
bool net_is_up();

// Which transport holds the lease — NetLink::None while nothing does.
NetLink net_kind();

// The netif carrying the default route, or nullptr when the link is down. Use this instead
// of esp_netif_get_handle_from_ifkey(): the ifkey differs per transport ("WIFI_STA_DEF" vs
// "ETH_DEF"), and a hardcoded one turns into a null deref the day the other is in use.
esp_netif_t* net_active_netif();

// Does this board HAVE an Ethernet interface at all — independent of whether it currently
// holds a lease? Answers the question net_kind() cannot while the link is still coming up:
// what is this device WAITING for. The display uses it to label its search animation
// ("LAN …" vs "WiFi …"); false on every board without the Ethernet backend compiled in.
bool net_eth_present();

// Cumulative RE-establishments of the link since boot; the first lease of a boot is NOT
// counted, so a healthy device reports 0 rather than a permanent 1 that would hide a real
// flap. Published as /status.sys.wifi_reconnects and as an MQTT diagnostic.
unsigned net_reconnect_count();

// The WiFi station's live RSSI in dBm. Returns false — leaving *rssi_dbm and ssid untouched —
// whenever the active transport is not WiFi, or the station record cannot be read safely.
// ssid must have room for 33 bytes (32 + NUL, per 802.11); pass nullptr if not wanted.
//
// The guard matters: esp_wifi_sta_get_ap_info() has transiently-null fields while the station
// is associating, and a concurrent read there faults (LoadProhibited, EXCVADDR=0x1). Callers
// must not re-derive that gate themselves.
bool net_wifi_signal(int* rssi_dbm, char* ssid);

// The Ethernet PHY's negotiated speed (10 / 100 Mbit) and duplex. Returns false — leaving
// both outputs untouched — unless Ethernet currently carries the lease. Surfaced as
// /status.eth; either pointer may be null.
bool net_eth_phy(int* speed_mbps, bool* full_duplex);

// A friendly 802.11 generation for the active WiFi link ("Wi-Fi 6", "802.11g", …), or
// nullptr when the transport is not WiFi. Surfaced as /status.wifi.std.
const char* net_wifi_standard();

// Start the connectivity watchdog task (logic/net_link.hpp's watch_step drives it). Call once
// the link is up; it probes the default gateway every ~30 s and, only on a PROVEN ghost link,
// forces the active transport to re-establish. It deliberately never reboots — a reboot during
// an outage would hit the boot-window timeout and drop into the setup portal, abandoning good
// credentials. Returns false if the task could not be created (an essential failure).
bool net_watchdog_start();

}  // namespace tk
