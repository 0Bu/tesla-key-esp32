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
    std::string addr;   // "aa:bb:cc:dd:ee:ff"
    std::string name;   // advertised local name, if any
    int8_t      rssi;   // dBm
};

class BleClient : public TeslaBLE::BleAdapter {
public:
    using ConnectedCb   = std::function<void(bool connected)>;
    using RxDataCb      = std::function<void(const std::vector<uint8_t>& data)>;

    BleClient();

    void set_connected_cb(ConnectedCb cb)  { on_connected_ = std::move(cb); }
    void set_rx_data_cb(RxDataCb cb)       { on_rx_data_   = std::move(cb); }
    // VIN of the vehicle to connect to; the scanner matches the Tesla BLE name
    // derived from it. Empty = connect to any Tesla in range.
    void set_target_vin(const std::string& vin) { target_vin_ = vin; }

    // Start NimBLE host + scanning task
    bool start();

    // Trigger scan → connect for the given MAC (stored in NVS config).
    // If mac is empty, scan for any Tesla device.
    void connect(const std::string& address) override;
    void disconnect() override;
    bool write(const std::vector<uint8_t>& data) override;

    bool is_connected() const { return conn_handle_ != BLE_HS_CONN_HANDLE_NONE; }
    std::string peer_addr_str() const;

    // RSSI (dBm) of the active connection; false if not connected / unavailable.
    bool connected_rssi(int8_t& out) const;
    // Snapshot of nearby Tesla vehicles seen while scanning (recent only).
    std::vector<TeslaScan> nearby() const;
    // Drop a pending connect intent (called when a command's connect attempt ends).
    void stop_connecting();
    // Start a manual, time-limited discovery scan (does not connect).
    void start_discovery(int ms);
    void on_scan_timeout();   // internal (timer callback)
    bool is_scanning() const { return scanning_; }

    // Called from NimBLE host task callbacks — not for external use
    void on_sync();
    int  on_gap_event(ble_gap_event* event);
    int  on_svc_disc(uint16_t conn_handle, const ble_gatt_error* error, const ble_gatt_svc* svc);
    int  on_chr_disc(uint16_t conn_handle, const ble_gatt_error* error, const ble_gatt_chr* chr);
    int  on_notify(uint16_t conn_handle, const ble_gatt_error* error,
                   ble_gatt_attr* attr);

private:
    void start_scan_();
    void ensure_scanning_();
    void note_scan_(const ble_gap_disc_desc& d, const ble_hs_adv_fields& f);
    void subscribe_notify_();
    void write_chunk_(const uint8_t* data, size_t len);

    // Discovery: nearby Teslas seen while not connected, and the connect intent.
    struct ScanEntry { uint8_t addr[6]; char name[24]; int8_t rssi; int64_t last_us; };
    std::vector<ScanEntry> scan_;
    SemaphoreHandle_t      scan_mutex_{nullptr};
    esp_timer_handle_t     scan_timer_{nullptr};
    volatile bool          want_connect_{false};
    volatile bool          connecting_{false};
    volatile bool          scanning_{false};

    ConnectedCb on_connected_;
    RxDataCb    on_rx_data_;

    uint16_t conn_handle_{BLE_HS_CONN_HANDLE_NONE};
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
