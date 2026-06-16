#include "ble_client.hpp"
#include <esp_log.h>
#include <cstring>
#include <algorithm>

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char* TAG = "ble_client";

// Singleton storage
static BleClient* g_instance = nullptr;
BleClient* ble_client_instance() { return g_instance; }

// ─── Static NimBLE callbacks ─────────────────────────────────────────────────

static void nimble_host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync_cb() {
    if (g_instance) g_instance->on_sync();
}

static void on_reset_cb(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static int gap_event_cb(ble_gap_event* event, void* arg) {
    auto* client = static_cast<BleClient*>(arg);
    return client->on_gap_event(event);
}

static int svc_disc_cb(uint16_t conn_handle, const ble_gatt_error* error,
                       const ble_gatt_svc* svc, void* arg) {
    auto* client = static_cast<BleClient*>(arg);
    return client->on_svc_disc(conn_handle, error, svc);
}

static int chr_disc_cb(uint16_t conn_handle, const ble_gatt_error* error,
                       const ble_gatt_chr* chr, void* arg) {
    auto* client = static_cast<BleClient*>(arg);
    return client->on_chr_disc(conn_handle, error, chr);
}

static int notify_cb(uint16_t conn_handle, const ble_gatt_error* error,
                     ble_gatt_attr* attr, void* arg) {
    auto* client = static_cast<BleClient*>(arg);
    return client->on_notify(conn_handle, error, attr);
}

// ─── BleClient ───────────────────────────────────────────────────────────────

BleClient::BleClient() {
    g_instance = this;
    write_mutex_ = xSemaphoreCreateMutex();
}

bool BleClient::start() {
    nimble_port_init();

    ble_hs_cfg.sync_cb  = on_sync_cb;
    ble_hs_cfg.reset_cb = on_reset_cb;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Prefer larger MTU to reduce fragmentation
    ble_att_set_preferred_mtu(247);

    int rc = ble_svc_gap_device_name_set("esp32-tesla-key");
    if (rc != 0) {
        ESP_LOGW(TAG, "device name set failed: %d", rc);
    }

    nimble_port_freertos_init(nimble_host_task);
    return true;
}

void BleClient::on_sync() {
    ESP_LOGI(TAG, "NimBLE synced");
    // Automatically start scanning if we have a configured address
    // (set by connect() call before sync). Otherwise wait for connect().
    if (has_target_) {
        ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &target_addr_,
                        30000, nullptr, gap_event_cb, this);
    } else {
        start_scan_();
    }
}

void BleClient::start_scan_() {
    ble_gap_disc_params params{};
    params.passive         = 0;
    params.filter_duplicates = 1;
    params.itvl            = 0x0010; // 10ms
    params.window          = 0x0010;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                           &params, gap_event_cb, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "scanning for Tesla BLE...");
    }
}

// BleAdapter::connect — called by Vehicle when it wants us to connect
void BleClient::connect(const std::string& address) {
    if (is_connected()) return;

    if (!address.empty()) {
        // Parse "AA:BB:CC:DD:EE:FF" string into ble_addr_t
        unsigned int b[6];
        if (sscanf(address.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                   &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) == 6) {
            target_addr_.type = BLE_ADDR_PUBLIC;
            for (int i = 0; i < 6; i++) target_addr_.val[i] = (uint8_t)b[i];
            has_target_ = true;
            ble_gap_disc_cancel();
            ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &target_addr_,
                            30000, nullptr, gap_event_cb, this);
            return;
        }
    }
    // No address — scan and connect to first Tesla found
    start_scan_();
}

void BleClient::disconnect() {
    if (conn_handle_ != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool BleClient::write(const std::vector<uint8_t>& data) {
    if (!is_connected() || write_handle_ == 0) return false;
    if (xSemaphoreTake(write_mutex_, pdMS_TO_TICKS(500)) != pdTRUE) return false;

    for (size_t offset = 0; offset < data.size(); offset += BLE_CHUNK_SIZE) {
        size_t chunk = std::min(BLE_CHUNK_SIZE, data.size() - offset);
        write_chunk_(data.data() + offset, chunk);
        if (offset + chunk < data.size()) {
            vTaskDelay(pdMS_TO_TICKS(10)); // small gap between chunks
        }
    }

    xSemaphoreGive(write_mutex_);
    return true;
}

void BleClient::write_chunk_(const uint8_t* data, size_t len) {
    int rc = ble_gattc_write_no_rsp_flat(conn_handle_, write_handle_, data, len);
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE write chunk failed: %d", rc);
    }
}

// ─── GAP event handler ────────────────────────────────────────────────────────

int BleClient::on_gap_event(ble_gap_event* event) {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        // Check if this device advertises Tesla service UUID
        ble_hs_adv_fields fields{};
        int rc = ble_hs_adv_parse_fields(&fields,
                                          event->disc.data,
                                          event->disc.length_data);
        if (rc != 0) break;

        bool found = false;
        for (int i = 0; i < fields.num_uuids128; i++) {
            if (ble_uuid_cmp(&fields.uuids128[i].u, &TESLA_SVC_UUID.u) == 0) {
                found = true;
                break;
            }
        }
        if (!found) break;

        // Found a Tesla vehicle — stop scan and connect
        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 event->disc.addr.val[5], event->disc.addr.val[4],
                 event->disc.addr.val[3], event->disc.addr.val[2],
                 event->disc.addr.val[1], event->disc.addr.val[0]);
        ESP_LOGI(TAG, "Tesla BLE found: %s", addr_str);
        peer_addr_str_ = addr_str;

        ble_gap_disc_cancel();
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC,
                             &event->disc.addr,
                             10000, nullptr,
                             gap_event_cb, this);
        if (rc != 0) {
            ESP_LOGE(TAG, "connect failed: %d", rc);
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connect error: %d", event->connect.status);
            if (on_connected_) on_connected_(false);
            start_scan_();
            break;
        }
        conn_handle_ = event->connect.conn_handle;
        ESP_LOGI(TAG, "connected, handle=%d", conn_handle_);

        // Discover Tesla service
        int rc = ble_gattc_disc_svc_by_uuid(conn_handle_,
                                             &TESLA_SVC_UUID.u,
                                             svc_disc_cb, this);
        if (rc != 0) {
            ESP_LOGE(TAG, "svc discovery failed: %d", rc);
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        conn_handle_       = BLE_HS_CONN_HANDLE_NONE;
        write_handle_      = 0;
        notify_handle_     = 0;
        notify_val_handle_ = 0;
        if (on_connected_) on_connected_(false);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle != notify_val_handle_) break;
        struct os_mbuf* om = event->notify_rx.om;
        if (!om) break;
        uint16_t pkt_len = OS_MBUF_PKTLEN(om);
        std::vector<uint8_t> buf(pkt_len);
        int rc = os_mbuf_copydata(om, 0, pkt_len, buf.data());
        if (rc == 0 && on_rx_data_) {
            on_rx_data_(buf);
        }
        break;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: %d", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

// ─── GATT service discovery ───────────────────────────────────────────────────

int BleClient::on_svc_disc(uint16_t conn_handle,
                            const ble_gatt_error* error,
                            const ble_gatt_svc* svc) {
    if (error->status == BLE_HS_EDONE) {
        if (svc_start_handle_ == 0) {
            ESP_LOGE(TAG, "Tesla service not found");
            disconnect();
            return 0;
        }
        // Service found — discover characteristics
        ble_gattc_disc_all_chrs(conn_handle,
                                 svc_start_handle_, svc_end_handle_,
                                 chr_disc_cb, this);
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGE(TAG, "svc disc error: %d", error->status);
        return 0;
    }
    if (svc && ble_uuid_cmp(&svc->uuid.u, &TESLA_SVC_UUID.u) == 0) {
        svc_start_handle_ = svc->start_handle;
        svc_end_handle_   = svc->end_handle;
        ESP_LOGD(TAG, "Tesla service: %d-%d", svc_start_handle_, svc_end_handle_);
    }
    return 0;
}

int BleClient::on_chr_disc(uint16_t conn_handle,
                            const ble_gatt_error* error,
                            const ble_gatt_chr* chr) {
    if (error->status == BLE_HS_EDONE) {
        if (write_handle_ == 0 || notify_val_handle_ == 0) {
            ESP_LOGE(TAG, "required characteristics not found (write=%d notify=%d)",
                     write_handle_, notify_val_handle_);
            disconnect();
            return 0;
        }
        ESP_LOGI(TAG, "BLE ready (write=%d notify=%d)",
                 write_handle_, notify_val_handle_);
        // Subscribe to notifications
        subscribe_notify_();
        return 0;
    }
    if (error->status != 0 || !chr) return 0;

    if (ble_uuid_cmp(&chr->uuid.u, &TESLA_WRITE_UUID.u) == 0) {
        write_handle_ = chr->val_handle;
        ESP_LOGD(TAG, "write chr: %d", write_handle_);
    } else if (ble_uuid_cmp(&chr->uuid.u, &TESLA_NOTIFY_UUID.u) == 0) {
        notify_handle_     = chr->def_handle;
        notify_val_handle_ = chr->val_handle;
        ESP_LOGD(TAG, "notify chr: val=%d", notify_val_handle_);
    }
    return 0;
}

void BleClient::subscribe_notify_() {
    // Write 0x0001 to the CCCD (descriptor at notify_val_handle + 1) to enable notifications
    uint16_t cccd_handle = notify_val_handle_ + 1;
    uint8_t  value[2]    = {0x01, 0x00}; // BLE_GATT_SUB_NOTIFY

    int rc = ble_gattc_write_flat(conn_handle_, cccd_handle,
                                   value, sizeof(value),
                                   nullptr, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "subscribe notify failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "subscribed to Tesla notifications");
    if (on_connected_) on_connected_(true);
}

int BleClient::on_notify(uint16_t, const ble_gatt_error*, ble_gatt_attr*) {
    return 0; // handled via BLE_GAP_EVENT_NOTIFY_RX
}
