#pragma once

#include <adapters.h>
#include <functional>
#include <vector>
#include <string>
#include <cstdint>

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

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

class BleClient : public TeslaBLE::BleAdapter {
public:
    using ConnectedCb   = std::function<void(bool connected)>;
    using RxDataCb      = std::function<void(const std::vector<uint8_t>& data)>;

    BleClient();

    void set_connected_cb(ConnectedCb cb)  { on_connected_ = std::move(cb); }
    void set_rx_data_cb(RxDataCb cb)       { on_rx_data_   = std::move(cb); }

    // Start NimBLE host + scanning task
    bool start();

    // Trigger scan → connect for the given MAC (stored in NVS config).
    // If mac is empty, scan for any Tesla device.
    void connect(const std::string& address) override;
    void disconnect() override;
    bool write(const std::vector<uint8_t>& data) override;

    bool is_connected() const { return conn_handle_ != BLE_HS_CONN_HANDLE_NONE; }
    std::string peer_addr_str() const { return peer_addr_str_; }

    // Called from NimBLE host task callbacks — not for external use
    void on_sync();
    int  on_gap_event(ble_gap_event* event);
    int  on_svc_disc(uint16_t conn_handle, const ble_gatt_error* error, const ble_gatt_svc* svc);
    int  on_chr_disc(uint16_t conn_handle, const ble_gatt_error* error, const ble_gatt_chr* chr);
    int  on_notify(uint16_t conn_handle, const ble_gatt_error* error,
                   ble_gatt_attr* attr);

private:
    void start_scan_();
    void subscribe_notify_();
    void write_chunk_(const uint8_t* data, size_t len);

    ConnectedCb on_connected_;
    RxDataCb    on_rx_data_;

    uint16_t conn_handle_{BLE_HS_CONN_HANDLE_NONE};
    uint16_t write_handle_{0};
    uint16_t notify_handle_{0};
    uint16_t notify_val_handle_{0};

    uint16_t svc_start_handle_{0};
    uint16_t svc_end_handle_{0};

    ble_addr_t target_addr_{};
    bool       has_target_{false};
    std::string peer_addr_str_;

    // Outbound write queue processed via direct NimBLE calls
    SemaphoreHandle_t write_mutex_{nullptr};
};

// Global instance used by static NimBLE callbacks
BleClient* ble_client_instance();
