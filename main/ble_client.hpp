#pragma once

#include <adapters.h>
#include <functional>
#include <vector>
#include <string>
#include <cstdint>
#include <esp_timer.h>

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// NimBLE's <nimble/ble.h> → <os/os.h> leaks function-like min()/max() macros
// that clobber libstdc++ <chrono>/<algorithm> and std::min/std::max. Undefine
// them here so every translation unit that pulls in this header stays clean.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// Tesla BLE GATT UUIDs
// Service:  00000211-b2d1-43f0-9b88-960cebf8b91e
// Write:    00000212-b2d1-43f0-9b88-960cebf8b91e
// Notify:   00000213-b2d1-43f0-9b88-960cebf8b91e
static const ble_uuid128_t TESLA_SVC_UUID = {
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
              0xf0, 0x43, 0xd1, 0xb2, 0x11, 0x02, 0x00, 0x00}
};
static const ble_uuid128_t TESLA_WRITE_UUID = {
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
              0xf0, 0x43, 0xd1, 0xb2, 0x12, 0x02, 0x00, 0x00}
};
static const ble_uuid128_t TESLA_NOTIFY_UUID = {
    .u = {.type = BLE_UUID_TYPE_128},
    .value = {0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
              0xf0, 0x43, 0xd1, 0xb2, 0x13, 0x02, 0x00, 0x00}
};

// Max BLE write chunk (ATT MTU 247 - 3 header = 244, but 20 is safest default)
static constexpr size_t BLE_CHUNK_SIZE = 20;

// A nearby Tesla vehicle seen while scanning (not connected).
struct TeslaScan {
    std::string addr;        // "aa:bb:cc:dd:ee:ff"
    std::string name;        // advertised local name, if any
    int8_t      rssi;        // dBm
    bool        connectable; // last-seen advert was connectable (false ⇒ car at its BLE
                             // connection limit — mirrors vehicle-command's Connectable check)
};

class BleClient : public TeslaBLE::BleAdapter {
public:
    using ConnectedCb   = std::function<void(bool connected)>;
    using RxDataCb      = std::function<void(const std::vector<uint8_t>& data)>;

    BleClient();

    void set_connected_cb(ConnectedCb cb)  { on_connected_ = std::move(cb); }
    void set_rx_data_cb(RxDataCb cb)       { on_rx_data_   = std::move(cb); }
    // VIN of the vehicle to connect to; the scanner matches the Tesla BLE name derived from
    // it. Empty = no target ⇒ nearby Teslas are still listed (/scan) but the scanner never
    // connects or enrols on one (the device must not pair onto an arbitrary nearby Tesla).
    void set_target_vin(const std::string& vin) { target_vin_ = vin; }

    // Start NimBLE host + scanning task
    bool start();

    // Set a connect intent; the running scan connects to the next advert matching the
    // configured target VIN (the address argument is unused — see the .cpp). With no target
    // VIN configured no connection is made.
    void connect(const std::string& address) override;
    void disconnect() override;
    bool write(const std::vector<uint8_t>& data) override;

    bool is_connected() const { return conn_handle_ != BLE_HS_CONN_HANDLE_NONE; }
    std::string peer_addr_str() const;

    // RSSI (dBm) of the active connection; false if not connected / unavailable.
    bool connected_rssi(int8_t& out) const;

    // Best-known advert RSSI of the target, valid even while NOT connected: it's seeded from
    // the advert on every connect attempt and the failed-connect path doesn't clear it (only a
    // real link-drop does). Lets the web UI show real bars + dBm in the "can't connect" state
    // instead of empty bars. false if nothing seen / the link genuinely dropped.
    bool last_advert_rssi(int8_t& out) const {
        if (!conn_rssi_valid_) return false;
        out = conn_rssi_;
        return true;
    }
    // Snapshot of nearby Tesla vehicles seen while scanning (recent only).
    std::vector<TeslaScan> nearby() const;
    // Drop a pending connect intent (called when a command's connect attempt ends).
    void stop_connecting();
    // Start a manual, time-limited discovery scan (does not connect).
    void start_discovery(int ms);
    void on_scan_timeout();   // internal (timer callback)
    bool is_scanning() const { return scanning_; }

    // Consecutive failed connects to the *target* car — its advert was heard and the
    // VIN-derived name matched, but ble_gap_connect timed out/errored so the link never came
    // up (e.g. another device is already holding the car's BLE connection). Reset on a
    // successful connect. Returns 0 once attempts stop (>30 s since the last one) so a car
    // that simply drove off reads as "out of range", not "failing". The web UI uses this to
    // tell "found the car but can't connect" apart from "looking for the car".
    uint32_t connect_fail_recent() const;

    // Connectability of the *target* car's most recent advert, mirroring how Tesla's official
    // vehicle-command derives ErrMaxConnectionsExceeded (it keys off the scan result's
    // Connectable flag, NOT the connect error — a vehicle at its BLE connection limit
    // advertises non-connectable). -1 = target not seen recently / not yet known,
    // 0 = advertising NON-connectable (≈ at its ~3-device BLE limit), 1 = connectable.
    int target_connectable() const;

    // Called from NimBLE host task callbacks — not for external use
    void on_sync();
    void on_reset();
    int  on_gap_event(ble_gap_event* event);
    int  on_svc_disc(uint16_t conn_handle, const ble_gatt_error* error, const ble_gatt_svc* svc);
    int  on_chr_disc(uint16_t conn_handle, const ble_gatt_error* error, const ble_gatt_chr* chr);

private:
    void start_scan_();
    void ensure_scanning_();
    void note_scan_(const ble_gap_disc_desc& d, const ble_hs_adv_fields& f);
    // Update a known scan entry's connectability by address — for primary adverts that carry
    // no name (Tesla puts the name in the SCAN_RSP, but connectability only on the primary
    // advert), so we record it by address and read it back once the name has matched.
    void note_connectable_(const ble_addr_t& addr, bool connectable);
    void subscribe_notify_();
    void write_chunk_(const uint8_t* data, size_t len);

    // Discovery: nearby Teslas seen while not connected, and the connect intent.
    struct ScanEntry { uint8_t addr[6]; char name[24]; int8_t rssi; int64_t last_us;
                       bool connectable; };  // connectable defaults true; flipped only on an
                                             // observed non-connectable primary advert
    std::vector<ScanEntry> scan_;
    SemaphoreHandle_t      scan_mutex_{nullptr};
    esp_timer_handle_t     scan_timer_{nullptr};
    volatile bool          want_connect_{false};
    volatile bool          connecting_{false};
    volatile bool          scanning_{false};
    // True only after the NimBLE host has signalled sync (on_sync). Until then ANY
    // ble_gap_* call hits an uninitialised host — a benign error on ESP-IDF 5.4 but a
    // null-deref crash (LoadProhibited) on 5.5. ble_client.start() runs only after WiFi
    // association (~4 s), which can lose the race with auto_pair's fixed 4 s warm-up, so
    // gate the scan on the real host state rather than on timing.
    volatile bool          host_synced_{false};

    ConnectedCb on_connected_;
    RxDataCb    on_rx_data_;

    uint16_t conn_handle_{BLE_HS_CONN_HANDLE_NONE};
    // Connect-failure tracking for the target car (see connect_fail_recent()). Stamped on
    // every connect attempt; the count climbs on each GAP connect error and resets on a
    // successful link. Both are touched only from the NimBLE host task / status reader.
    volatile uint32_t connect_fail_count_{0};
    volatile int64_t  last_connect_attempt_us_{0};
    uint16_t write_handle_{0};
    uint16_t notify_handle_{0};
    uint16_t notify_val_handle_{0};

    // Last-known link RSSI: seeded from the advert we connected to, then refreshed by
    // every successful live read in connected_rssi(). The live HCI "Read RSSI" can fail
    // transiently (e.g. while the controller is busy pairing), so this fallback keeps the
    // web UI showing real signal strength during pairing instead of nothing. mutable: the
    // refresh happens inside the const connected_rssi() accessor.
    mutable int8_t conn_rssi_{0};
    mutable bool   conn_rssi_valid_{false};

    uint16_t svc_start_handle_{0};
    uint16_t svc_end_handle_{0};

    ble_addr_t target_addr_{};
    bool       has_target_{false};
    std::string peer_addr_str_;
    std::string target_vin_;

    // Outbound write queue processed via direct NimBLE calls
    SemaphoreHandle_t write_mutex_{nullptr};
    SemaphoreHandle_t client_mutex_{nullptr};
};

// Global instance used by static NimBLE callbacks
BleClient* ble_client_instance();
