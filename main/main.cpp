#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <string>
#include <atomic>
#include <exception>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "esp_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "bootloader_random.h"

#include "boot_fatal.hpp"
#include "net.hpp"
#include "ble_client.hpp"
#include "nvs_storage.hpp"
#include "task_config.hpp"
#include "vehicle_ctrl.hpp"
#include "http_server.hpp"
#include "provisioning.hpp"
#include "diag_log.hpp"
#include "diag_crash.hpp"
#include "safe_mode.hpp"
#include "config_blob.hpp"
#include "ota_update.hpp"
#include "mqtt_ha.hpp"
#include "syslog.hpp"
#include "display.hpp"
#include "led_status.hpp"
#include "logic/bootlog.hpp"
#include "logic/health_gate.hpp"

static const char* MDNS_HOSTNAME = "tesla-key-esp32";  // → http://tesla-key-esp32.local

static const char* TAG = "main";

// An essential startup failure is permanent for the current boot. A pending OTA image must
// actively roll back; merely parking the task would leave the device wedged on the unverified
// slot until somebody resets it. An already-valid image is halted instead of automatically
// rebooted, because a reboot loop repeatedly opens the vehicle polling window while erasing the
// most useful in-memory diagnostic context.
[[noreturn]] void boot_fatal(const char* component) {
    ESP_LOGE(TAG, "FATAL: essential component '%s' failed to initialize; refusing to run a "
                  "partial firmware", component);

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state{};
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "fatal startup failure on pending OTA image — rolling back");
        vTaskDelay(pdMS_TO_TICKS(250));
        const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "OTA rollback could not be started: %s", esp_err_to_name(err));
    }

    ESP_LOGE(TAG, "valid image halted after fatal startup failure; external reset required");
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}

// ─── Wall clock ───────────────────────────────────────────────────────────────
// NTP (esp_sntp) is the primary time source; the browser (POST /set_time) is only a
// fallback for networks that block NTP. on_time_sync() flips s_ntp_synced and, on the
// first sync, refreshes the NVS cache so a later offline reboot restores a recent
// accurate time instead of sitting at 1970.
// atomic (not volatile): written from the SNTP callback task, read from the http/set_time
// task. volatile stops selected compiler optimizations but is NOT a cross-task
// happens-before edge under the C++ memory model; std::atomic is. Simple seq_cst policy.
static std::atomic<bool>  s_ntp_synced{false};
static NvsStorageAdapter* s_cfg_store  = nullptr;

static void on_time_sync(struct timeval*) {
    const bool first_sync = !s_ntp_synced.exchange(true);
    if (first_sync && s_cfg_store) {
        try {
            if (!s_cfg_store->save_str("last_time", std::to_string((long long)time(nullptr)))) {
                ESP_LOGW(TAG, "NTP time synced but not cached to NVS — a headless reboot with "
                              "NTP unreachable will come up at 1970");
            }
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "NTP callback could not cache time (%s)", e.what());
        } catch (...) {
            ESP_LOGE(TAG, "NTP callback could not cache time (unknown exception)");
        }
    }
    ESP_LOGI(TAG, "NTP time synced");
}

// Queried by the HTTP /set_time handler so the browser clock is applied only as a
// fallback while NTP has not synced this boot.
bool clock_synced_via_ntp() { return s_ntp_synced.load(); }

// Seed the wall clock from the NVS cache written by on_time_sync, so we never sit at 1970
// waiting for NTP (or forever, if the network blocks it and no browser ever visits). Called
// early in app_main — before VehicleController::init, whose persisted-session age check is
// wrong at 1970; see the call site for the underflow this prevents. Network-free by design:
// it must be usable before esp_netif exists. Refined by NTP as soon as the link is up.
static void restore_clock_from_nvs(NvsStorageAdapter& config_store) {
    std::string last_time;
    if (!config_store.load_str("last_time", last_time) || last_time.empty()) {
        ESP_LOGW(TAG, "no cached clock in NVS — starting at 1970 until NTP syncs; persisted "
                      "BLE sessions will be rejected as stale for this boot");
        return;
    }
    struct timeval tv = { (time_t)atoll(last_time.c_str()), 0 };
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "clock restored from NVS: %s (NTP will refine it)", last_time.c_str());
}

// ─── app_main ─────────────────────────────────────────────────────────────────

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW(ota/restart)";
        case ESP_RST_PANIC:     return "PANIC(abort/exception)";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// Heap-attribution probe: logs free + largest contiguous block at each init milestone.
// The LARGEST free block (not total free) is what bounds big allocations (OTA TLS record
// buffers, the tesla-ble session), so it's the number that decides whether the device
// OOM-crashes. Measured budget on this board: WiFi −57 KB, NimBLE −86 KB (dominant),
// HTTP −12 KB, MQTT −20 KB of largest-block — handy when tuning the footprint further.
static void log_heap(const char* where) {
    ESP_LOGW(TAG, "HEAP @%-9s free=%u largest=%u min=%u", where,
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

// OTA rollback health gate. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE leaves a freshly-flashed
// image in ESP_OTA_IMG_PENDING_VERIFY until the app calls
// esp_ota_mark_app_valid_cancel_rollback(); if the device reboots before that call, the
// bootloader reverts to the previous slot. We defer the call to this task so the new image
// has to prove itself first — catching a "boots fine, then crashes/OOM-reboots under load"
// image, which the old mark-at-startup placement would have already committed.
//
// What counts as proof is decided by logic/health_gate.hpp, and it is CONNECTIVITY plus a
// minimum uptime — not uptime alone. An image that boots perfectly and never gets on the
// network is exactly the image no OTA can fix afterwards, and it survives any pure timer
// without difficulty; the old 90-second sleep sealed that image in as valid and spent the
// rollback that would have undone it. No-op on a normal (non-pending) boot.
//
// Whether being online is even the expected state is the caller's fact, sampled once at arm
// time: a device with no credentials and no wire is legitimately offline. On this firmware
// that state never reaches here (the setup portal does not return), which is why the flag is
// computed rather than assumed — it is the control flow around it that would change.
static bool s_ota_gate_link_expected = false;

static void ota_health_gate_task(void*) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(running, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelete(nullptr);   // normal boot — nothing pending to confirm
        return;
    }

    ESP_LOGI(TAG, "OTA image pending verify — rollback stays armed until the link is proven "
                  "(min %us, giving up after %us)",
             (unsigned) tk::kHealthGateBaseS, (unsigned) tk::kHealthGateCapS);

    // Poll rather than sleep-then-decide: the verdict is a function of elapsed time AND a link
    // that can appear at any moment, so the commit should land shortly after the evidence does.
    constexpr uint32_t kPollS = 5;
    const TickType_t   start  = xTaskGetTickCount();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollS * 1000));
        const uint32_t elapsed_s =
            (uint32_t) (pdTICKS_TO_MS(xTaskGetTickCount() - start) / 1000);
        const tk::HealthVerdict v =
            tk::health_gate_decide(elapsed_s, tk::kHealthGateBaseS, tk::kHealthGateCapS,
                                   s_ota_gate_link_expected, tk::net_is_up());
        if (v == tk::HealthVerdict::Wait) continue;

        if (v == tk::HealthVerdict::Commit) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
                ESP_LOGI(TAG, "OTA image healthy after %us on %s — marked valid (rollback "
                              "cancelled, largest block %u)",
                         (unsigned) elapsed_s, tk::net_link_str(tk::net_kind()),
                         (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        } else {
            // Loud, and to syslog as well as /diag: this is the one outcome where the device
            // keeps running a build that is going to disappear on the next reboot, and nothing
            // else in the system will mention it.
            ESP_LOGE(TAG, "OTA image never got a link within %us — NOT marking it valid; the "
                          "next reboot rolls back to the previous firmware. Save any setting "
                          "(e.g. POST /set_mqtt) to keep this image instead.",
                     (unsigned) tk::kHealthGateCapS);
        }
        break;
    }
    vTaskDelete(nullptr);
}

extern "C" void app_main() {
  // Top-level exception boundary (issue #204): app_main runs C++ that allocates (std::string
  // config, make_unique, the component start()s). An uncaught throw would unwind into the C
  // startup that invoked app_main → std::terminate → abort — the same reboot, but with no
  // diagnostic. Contain it, log it, then roll back a pending image or halt a valid one.
  try {
    // Capture console output into the in-memory diagnostic ring (GET /diag).
    diag_log_init();

    // Why we (re)booted, plus the heap baseline to pair it with. SAMPLED HERE, LOGGED LATER:
    // the line itself is emitted after syslog_start() below, because syslog_send() is a no-op
    // until then and this line would otherwise never leave the device. It did not, for the
    // whole of 17.-24.07.2026: 56 boots, zero `BOOT reset_reason=` lines at the collector,
    // which is what made the unattended 20.07. reboot (1h54m of silence, then a boot)
    // impossible to explain afterwards — /diag is RAM and does not survive the restart.
    // The values must still be read HERE, before NVS init, WiFi, the syslog queue and the
    // component start()s have allocated: sampled after all that, "free heap at boot" would
    // describe a boot that already happened rather than the state we came up in.
    const char*    boot_reason      = reset_reason_str(esp_reset_reason());
    const unsigned boot_free        = (unsigned) esp_get_free_heap_size();
    const unsigned boot_min_free    = (unsigned) esp_get_minimum_free_heap_size();
    const unsigned boot_largest     = (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    // Capture WHAT the last run left behind, immediately after the heap baseline above and before
    // anything else allocates: the reset reason (always) plus, where the coredump partition exists,
    // the dump SUMMARY — crashed task, PC, backtrace, the build that wrote it. Parsing it costs a
    // ~2 KB transient allocation, which is why it belongs here rather than on a request path where
    // the heap is already committed to WiFi + NimBLE + MQTT. Everything downstream — /status,
    // safe mode, the syslog boot replay — reads this one cache.
    tk::diag_crash_capture();

    // NimBLE logs every GAP/GATT procedure at INFO — tens of lines per connect.
    // That noise buries the pairing/key-lifecycle messages in /diag (and fills the
    // ring fast). Raise its threshold to WARN so /diag reads as a clean lifecycle log;
    // our own components (vehicle_ctrl, ble_client, …) keep logging at INFO.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Static so they outlive app_main() (which deletes itself via vTaskDelete)
    static NvsStorageAdapter config_store("tesla_cfg");
    if (!config_store.initialize())
        boot_fatal("configuration NVS");

    // Did WE end the last boot on purpose? esp_reset_reason() cannot tell a deliberate
    // esp_restart() apart from a user power-cycle — both read SW/POWERON — so the heap watchdog
    // leaves a breadcrumb in NVS on its way out. Take it (read + clear) before anything else can
    // reboot, so it always describes the boot just made, and surface it in /status: a device that
    // self-healed at 04:00 must be able to say so, or the next investigation starts from scratch
    // exactly the way this one did.
    VehicleController::set_boot_reboot_reason(VehicleController::take_reboot_reason(config_store));

    // Boot-loop guard. The heap watchdog bounds the restarts IT chooses; this bounds the ones the
    // SYSTEM forced — a panic, a brownout or a task-watchdog loop, none of which were counted by
    // anything before. Past the threshold it latches safe mode and app_main below brings up WiFi +
    // web UI + OTA ONLY, so a device that crashes on the vehicle path stays reachable in a browser
    // instead of needing a USB cable. It also stops each boot re-opening the car's polling window,
    // which is what turns a reboot loop into a flat traction battery.
    // A deliberate esp_restart() (a /set_* save, an OTA) reports ESP_RST_SW and is NOT a fault, so
    // ordinary reboots never count toward it.
    const bool safe_mode = tk::safe_mode_begin(config_store, tk::diag_crash_info().fault);

    // UDP Syslog forwarder for the diag log (NVS "syslog_uri" / CONFIG_TESLA_SYSLOG_SERVER;
    // "" = disabled). Started before the network so it captures boot-time log lines too — its
    // own task blocks on tk::net_is_up() until the link is up, so this is safe even though
    // esp_netif_init() itself hasn't run yet (that happens in tk::net_init() below).
    // OPTIONAL subsystem: a false return (resources/task couldn't be allocated) leaves
    // forwarding disabled and degraded, but never stops boot — syslog_start() has already
    // logged the specific reason.
    if (!syslog_start(config_store))
        ESP_LOGW(TAG, "Syslog forwarding is degraded/disabled (see the error above)");

    // FIRST line past the forwarder, because it is the one a post-mortem starts from: syslog
    // is the only record that outlives a restart. PANIC = abort()/uncaught C++ exception,
    // BROWNOUT = power dip, *_WDT = a stuck task; the heap figures next to it are what
    // distinguish an OOM-driven abort from a clean restart. Values sampled at entry (above).
    ESP_LOGW(TAG, "BOOT reset_reason=%s free_heap=%u min_free=%u largest_block=%u",
             boot_reason, boot_free, boot_min_free, boot_largest);

    // Announce the breadcrumb only NOW, deliberately after syslog_start(): syslog_send() is a
    // no-op until then (diag_log.cpp's capture hook has nowhere to forward to), so logging it
    // above — where the value is read — would confine the one line explaining an unattended
    // self-heal to the /diag RAM ring, which the next restart erases. Queued lines survive until
    // WiFi is up, so this does reach the collector. The read itself stays above, before anything
    // else can reboot, so the value always describes the boot just made.
    if (!VehicleController::boot_reboot_reason().empty()) {
        ESP_LOGW(TAG, "BOOT this boot was caused by the firmware itself: reason=%s — the previous "
                      "run restarted deliberately because its heap stayed unusable (also in "
                      "/status as last_reboot; see docs/ARCHITECTURE.md)",
                 VehicleController::boot_reboot_reason().c_str());
    }

    // Resolve WiFi credentials: the atomic config blob (falling back to the legacy per-key layout
    // on a device that has not saved since upgrading) overrides the Kconfig defaults.
    static tk::ConfigBlob cfg_blob;
    tk::cfg_load(config_store, cfg_blob);
    static std::string ssid     = cfg_blob.wifi_ssid.empty() ? CONFIG_TESLA_WIFI_SSID
                                                             : cfg_blob.wifi_ssid;
    static std::string password = cfg_blob.wifi_ssid.empty() ? CONFIG_TESLA_WIFI_PASSWORD
                                                             : cfg_blob.wifi_pass;

    // Is there a wire? Probed HERE, before the setup-portal decision and before anything else
    // has touched a GPIO — it reads one W5500 identity register and does not wait for a link or
    // a lease, so it costs a few hundred microseconds. On a board with no controller (or on the
    // T-Dongle-S3, whose panel owns the SPI clock pin) it frees the bus again and reports false.
    const bool have_wire = tk::net_eth_probe();

    // The setup portal exists because a device with no credentials has no other way to be
    // reached. A WIRED device does: DHCP gives it an address with nothing configured at all, so
    // sending it to a captive AP would strand a perfectly reachable board — a regression created
    // purely by adding a transport. The VIN is then set over the LAN like any other setting.
    if (ssid.empty() && !have_wire) {
        ESP_LOGW(TAG, "No WiFi configured — starting setup portal (join WiFi '%s')",
                 "tesla-key-esp32-setup");
        provisioning_run(config_store);  // never returns; reboots on save
    }
    if (ssid.empty())
        ESP_LOGI(TAG, "no WiFi configured, but an Ethernet controller is present — coming up "
                      "on the wire (set the VIN in the web UI at the DHCP address)");

    // Resolve VIN
    static std::string vin = CONFIG_TESLA_VIN;
    config_store.load_str("vin", vin);
    if (vin.empty()) {
        ESP_LOGW(TAG, "VIN not configured — pairing disabled until a VIN is set "
                      "(setup AP or POST /set_vin / CONFIG_TESLA_VIN / NVS key 'vin'). "
                      "Nearby Teslas are still listed by /scan, but none is connected/enrolled.");
        vin = "UNKNOWN";  // placeholder for display/logging only — kept out of the BLE matching path
    }

    // Resolve BLE MAC (persisted after first successful scan)
    static std::string ble_mac = CONFIG_TESLA_BLE_MAC;
    config_store.load_str("ble_mac", ble_mac);

    // Physical board identity is diagnostic evidence, not the HA identity (which follows VIN).
    uint8_t board_mac[6] = {0};
    char board_mac_text[18] = "unavailable";
    if (esp_read_mac(board_mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        std::snprintf(board_mac_text, sizeof(board_mac_text), "%02x:%02x:%02x:%02x:%02x:%02x",
                      (unsigned)board_mac[0], (unsigned)board_mac[1], (unsigned)board_mac[2],
                      (unsigned)board_mac[3], (unsigned)board_mac[4], (unsigned)board_mac[5]);
    }
    ESP_LOGI(TAG, "VIN: %s  BLE MAC: %s  Board MAC: %s", vin.c_str(),
             ble_mac.empty() ? "(scan)" : ble_mac.c_str(), board_mac_text);

    log_heap("preinit");

    // Wall clock, restored from NVS — BEFORE VehicleController::init below, which is the whole
    // point of doing it here rather than next to the SNTP setup after WiFi (where it used to
    // live). init() hands the persisted BLE sessions to tesla-ble, which validates their age as
    //     session_age = (uint32_t) time(nullptr) - session.clock_time      (vehicle.cpp:1123)
    // and rejects anything older than an hour. Run at 1970 that subtraction underflows, so the
    // age comes out as the raw stored epoch and EVERY persisted session is discarded: 49 boots
    // in the 17.-24.07.2026 syslog, 49 rejections of both domains. The last one threw away a
    // VCSEC session that was 43 minutes old — comfortably inside the library's own window —
    // and paid a fresh handshake for it, which is exactly what NVS `sess_vcsec`/`sess_info`
    // exist to avoid.
    //
    // Needs no network (unlike SNTP, which stays below with the rest of the post-WiFi setup),
    // so there is nothing keeping it down there. NTP refines this within seconds of the link
    // coming up; until then a cached-but-slightly-stale clock beats 1970 for every consumer —
    // session ages here, TLS cert validity for OTA, and the key_created/paired_at stamps.
    //
    // If you are about to move this back down: the comment that used to sit next to it said
    // "tesla-ble signed-command freshness does NOT [need real UTC]", which is true — signing
    // uses the vehicle's SessionInfo.ClockTime plus a monotonic delta (peer.cpp) — and is
    // exactly the sentence that made this look safe. Session PERSISTENCE is a different
    // consumer with a different clock, and it is the one that breaks.
    restore_clock_from_nvs(config_store);

    // ── Tesla BLE controller ─────────────────────────────────────────────────
    // Construct the controller (NVS + key) here; NimBLE itself (ble_client.start)
    // is started after WiFi is up. The controller's accessors are safe to call
    // before that — they report "not connected" until the link comes up.
    static NvsStorageAdapter tesla_store("tesla_ble");
    if (!tesla_store.initialize())
        boot_fatal("Tesla NVS");
    static BleClient ble_client;
    static VehicleController vehicle;
    // init() wires the connected + rx callbacks onto ble_client and passes the
    // config_store so it can save the discovered MAC. ESSENTIAL: without the controller
    // there is no BLE proxy at all, so a failed init halts boot (and leaves any pending OTA
    // image unconfirmed → rolled back).
    // In safe mode the controller is still fully WIRED (so /status, the web UI and the MQTT
    // snapshot read a coherent object) but its two background tasks are not started — see
    // vehicle_ctrl.cpp. Skipping init() altogether is the wrong shape: the HTTP server below takes
    // this controller by reference and would then read a half-constructed one.
    if (!vehicle.init(vin, ble_client, tesla_store, config_store, ble_mac, /*start_tasks=*/!safe_mode))
        boot_fatal("VehicleController");

    // Create the ECDSA key on first boot so a key always exists (and a fingerprint
    // is shown). Regeneration is an explicit, confirmed action in the web UI; this
    // never overwrites an existing key — only generates when none is present.
    if (!vehicle.has_key()) {
        ESP_LOGI(TAG, "no key in storage — generating initial key");
        // The ESP32 hardware RNG only returns TRUE random numbers while an entropy source is
        // active: RF (WiFi/BT) enabled, the bootloader running, or bootloader_random_enable()
        // (SAR-ADC entropy). Neither WiFi (wifi_connect) nor BLE (ble_client.start) is up yet,
        // so without this the EC private key — the device's sole authenticator to the car and
        // the OTA trust root — would be seeded from PSEUDO-random data (tesla-ble seeds its DRBG
        // once and reuses it, so same-boot re-keys inherit the weak seed). Enable the SAR-ADC
        // entropy source for the key generation, then disable it again before WiFi/ADC start
        // (Espressif's documented pattern for "true random before RF is up").
        bootloader_random_enable();
        bool key_ok = vehicle.generate_key();
        bootloader_random_disable();
        if (key_ok) {
            ESP_LOGI(TAG, "initial key generated, fingerprint %s",
                     vehicle.key_fingerprint().c_str());
        } else {
            ESP_LOGE(TAG, "initial key generation failed");
        }
    } else {
        ESP_LOGI(TAG, "key present, fingerprint %s", vehicle.key_fingerprint().c_str());
    }
    // Match by the VIN-derived BLE name on scan. Pass the real VIN only when it is a plausible
    // 17-char VIN; with none configured we pass an EMPTY target so the scanner lists nearby
    // Teslas but never connects/enrols on one. The "UNKNOWN" placeholder must stay out of the
    // matching path — it would hash to a name that just happens never to collide, making the
    // safe outcome accidental rather than designed. Pairing is gated on a real VIN.
    ble_client.set_target_vin(vehicle.has_plausible_vin() ? vin : std::string{});

    // ── network: the wire first, the radio second ────────────────────────────
    // Ethernet is preferred whenever it can actually carry the lease, and the reason is not
    // bandwidth — it is the radio. WiFi and BLE share ONE antenna path on every chip this
    // firmware targets, so a running WiFi stack means time-division coexistence with NimBLE for
    // as long as the device is powered; that is what forces WIFI_PS_MIN_MODEM and what makes
    // every GATT round-trip to the car slower than it needs to be. Coming up on a wire does not
    // merely avoid using WiFi, it avoids STARTING it: no coexistence arbitration at all, and the
    // ~57 KB of largest-block the stack holds stays free on a device whose binding limit is the
    // largest CONTIGUOUS block.
    //
    // A controller with no cable falls through to WiFi rather than stranding the board, and its
    // driver keeps running, so a cable plugged in later still takes over.
    const bool on_wire = have_wire && tk::net_start_eth();
    if (on_wire) log_heap("eth");

    if (!on_wire && ssid.empty()) {
        // The wire was present at probe time but never got a lease, and there are no credentials
        // to fall back to. Rebooting would just repeat this; the portal at least makes the device
        // configurable by someone standing next to it.
        ESP_LOGE(TAG, "Ethernet present but not usable and no WiFi configured — setup portal");
        provisioning_run(config_store);  // never returns; reboots on save
    }

    // Connect to WiFi. With stored credentials, a failure is usually a transient
    // outage (e.g. router rebooting), but if it persists (e.g. wrong password),
    // fallback to the setup portal so the user can reconfigure it.
    if (!on_wire &&
        !tk::net_start_wifi(ssid.c_str(), password.c_str(), cfg_blob.wifi_rollback_active)) {
        if (cfg_blob.wifi_rollback_active) {
            // The credentials from the last /set_wifi did not work and the grace window is spent.
            // Restore the pair that DID work and reboot onto it, rather than dropping into the
            // setup portal — which would require someone to be standing next to the device, the
            // exact situation being able to change WiFi over the LAN exists to avoid.
            ESP_LOGE(TAG, "WiFi credential change failed — restoring the previous network and "
                          "rebooting (reported on /status as wifi.rolled_back)");
            cfg_blob.wifi_ssid = cfg_blob.wifi_ssid_backup;
            cfg_blob.wifi_pass = cfg_blob.wifi_pass_backup;
            cfg_blob.wifi_ssid_backup.clear();
            cfg_blob.wifi_pass_backup.clear();
            cfg_blob.wifi_rollback_active = false;
            cfg_blob.wifi_rolled_back     = true;   // the ONLY trace: the reboot shows the old SSID
            if (tk::cfg_save(config_store, cfg_blob)) {
                ota_confirm_pending_image();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
            // The restore lives in NVS alone, so an unpersisted one would be re-decided identically
            // on every boot — a reboot loop we cannot write our way out of. Fall through to the
            // portal instead, which at least leaves the device configurable.
            ESP_LOGE(TAG, "could not persist the credential rollback — falling back to the setup "
                          "portal rather than rebooting into a loop");
        }
        // LAST look before surrendering the running firmware to the portal. The Ethernet
        // driver keeps polling through the whole WiFi boot window, so a cable plugged in
        // during those ~30 s can have taken the link while this branch was being reached —
        // and dropping a working, reachable device into a captive AP would be the worst
        // possible answer to "the network is up".
        if (tk::net_is_up()) {
            ESP_LOGW(TAG, "WiFi did not come up, but the link is carried by %s — staying up",
                     tk::net_link_str(tk::net_kind()));
        } else {
            ESP_LOGW(TAG, "WiFi connection failed — starting setup portal");
            provisioning_run(config_store); // never returns; reboots on save
        }
    }
    // Associated on the new credentials: the trial is over and the backup has done its job. Drop it
    // so a LATER, unrelated outage can never restore credentials from months ago.
    //
    // NOT on the wired path: coming up on Ethernet proves nothing about the credentials on
    // trial, so consuming the backup there would silently discard the only way back to a working
    // network the moment the cable is unplugged.
    if (!on_wire && cfg_blob.wifi_rollback_active) {
        cfg_blob.wifi_ssid_backup.clear();
        cfg_blob.wifi_pass_backup.clear();
        cfg_blob.wifi_rollback_active = false;
        if (!tk::cfg_save(config_store, cfg_blob))
            ESP_LOGW(TAG, "WiFi credentials are good but the one-shot backup was not cleared");
    }
    if (!on_wire) log_heap("wifi");

    // mDNS: advertise http://tesla-key-esp32.local so users need not find the IP
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(MDNS_HOSTNAME);
        mdns_instance_name_set("tesla-key-esp32");
        // TXT records so discovery tools (dns-sd -B / avahi-browse / our own /scan)
        // can tell multiple devices apart without first resolving each .local host.
        // mdns copies these internally, so the pointers need only outlive the call.
        mdns_txt_item_t txt[] = {
            { "vin", vin.c_str() },
            { "ver", esp_app_get_description()->version },
        };
        mdns_service_add(nullptr, "_http", "_tcp", 80, txt, 2);
        ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOSTNAME);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }

    // SNTP takes over the wall clock from here (the NVS restore already ran before
    // VehicleController::init, see restore_clock_from_nvs above). On sync it refreshes the NVS
    // cache and supersedes any restored or browser-supplied (POST /set_time) value.
    s_cfg_store = &config_store;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();

    // Start NimBLE host. Discovery scanning is manual/time-limited; the client
    // connects on demand when a command is issued. (The controller was set up
    // before WiFi, above.) ESSENTIAL.
    // Not in safe mode: NimBLE is the largest, most allocation-heavy subsystem here and the one
    // every vehicle code path runs through, so a crash loop that safe mode is meant to break out of
    // is far more likely to live behind it than in the web server. Leaving the host down also frees
    // the heap it would hold, which is what makes the recovery surface (UI + OTA) comfortable.
    if (!safe_mode) {
        if (!ble_client.start())
            boot_fatal("NimBLE");
        log_heap("ble");
    } else {
        ESP_LOGW(TAG, "SAFE MODE — NimBLE not started");
    }

    // Primary HTTP API (evcc + web UI + MCP). ESSENTIAL: it is the device's whole reason to
    // exist. http_server_start() unwinds a partial registration internally and returns false;
    // halt boot on that so we never announce a server that answers some routes and 404s others.
    if (!http_server_start(vehicle, config_store))
        boot_fatal("HTTP server");
    log_heap("http");

    // Home Assistant MQTT bridge: publishes all telemetry + device status (read-only)
    // if a broker is configured (NVS "mqtt_uri" / CONFIG_TESLA_MQTT_BROKER_URI); a
    // no-op otherwise. Runs in its own task, independent of evcc/BLE/pairing. OPTIONAL:
    // a failed start degrades to disabled (logged) without stopping boot.
    // Not in safe mode: with the vehicle loop down there is nothing fresh to publish, and the
    // bridge's TLS path is a second large allocator competing with the OTA that is the whole point
    // of staying reachable. HA sees the LWT go offline, which is the honest signal.
    if (!safe_mode) {
        if (!mqtt_ha_start(vehicle, config_store))
            ESP_LOGW(TAG, "MQTT bridge is degraded/disabled (see the error above)");
        log_heap("mqtt");
    } else {
        ESP_LOGW(TAG, "SAFE MODE — MQTT bridge not started");
    }

    // On-device status display (LilyGo T-Dongle-S3). No-op unless the board
    // build selects CONFIG_TESLA_DISPLAY_ENABLED — and on esp32s3 also a no-op unless the
    // T-Dongle-S3 is auto-detected (a generic ESP32-S3 has no panel). Reads only cached
    // state (never wakes the car) in its own task, so it can't queue behind a BLE poll.
    display_start(vehicle);
    log_heap("display");

    // On-device status LED (LilyGo T-Dongle underside APA102). No-op unless the board build
    // selects CONFIG_TESLA_LED_ENABLED; reads only cached state via the same UiSnapshot the
    // display uses (never wakes the car), independent of the display / MQTT.
    led_status_start(vehicle);
    log_heap("led");

    // LAN connectivity watchdog: forces the active transport to re-establish if the link
    // silently dies, including the "ghost association" case that fires no disconnect event
    // (net.cpp; the decision logic is logic/net_link.hpp). Without it the device can sit
    // reachable-over-BLE but off the LAN indefinitely, recoverable only by a manual reset.
    // ESSENTIAL: without the watchdog a silent LAN drop can strand the device off the network
    // with no automatic recovery — so a failure to create it halts boot rather than run
    // without the safety net.
    if (!tk::net_watchdog_start())
        boot_fatal("LAN watchdog");

    // Confirm a freshly OTA-flashed image only after it has proven it can RUN — not merely
    // reach this line. Marking it valid here (the old behaviour) would disarm rollback the
    // instant the tasks start, so an image that boots but then crashes/OOM-reboots only under
    // load would already have cancelled its safety net. The health-gate task instead holds
    // rollback armed for a window of healthy uptime; if the image dies first it reboots while
    // still PENDING_VERIFY and the bootloader reverts to the previous slot. A no-op on a normal
    // (non-pending-verify) boot.
    // ESSENTIAL for the safety of a freshly-flashed image: this is the task that eventually
    // marks a PENDING_VERIFY OTA image valid after a healthy window. If it cannot even be
    // created, the image would sit unconfirmed forever; boot_fatal() explicitly rolls a pending
    // image back to the last-good slot and halts an already-valid image.
    //
    // Is being online the expected state? Only if this device has a route to lose. Sampled here,
    // where both facts are in scope, rather than re-derived inside the task from state that other
    // code is free to change underneath it.
    s_ota_gate_link_expected = !ssid.empty() || on_wire;
    if (xTaskCreate(ota_health_gate_task, "ota_gate", 3072, nullptr,
                    tk::kPrioOtaGate, nullptr) != pdPASS)
        boot_fatal("OTA health gate");

    // Clear the crash-boot counter once THIS boot has proven it can stay up under load. A timer,
    // not a line at the end of app_main: reaching here proves the device initialised, while the
    // crashes this guards against happen minutes in — an end-of-init clear would have declared
    // exactly those boots healthy and the counter would never reach its threshold.
    tk::safe_mode_arm_healthy_timer(config_store);

    if (safe_mode) {
        ESP_LOGW(TAG, "tesla-key-esp32 running in SAFE MODE on port 80 — web UI, /status and OTA "
                      "are up; BLE, pairing, commands and MQTT are DOWN. Fix the configuration or "
                      "install a newer build, then reboot.");
    } else {
        ESP_LOGI(TAG, "tesla-key-esp32 running. API on port 80.");
    }
    // Main task is no longer needed; Vehicle loop + HTTP server run in their own tasks.
    vTaskDelete(nullptr);
  } catch (const std::exception& e) {
      ESP_LOGE(TAG, "app_main initialization threw (%s)", e.what());
      boot_fatal("app_main exception boundary");
  } catch (...) {
      ESP_LOGE(TAG, "app_main initialization threw (unknown)");
      boot_fatal("app_main exception boundary");
  }
}
