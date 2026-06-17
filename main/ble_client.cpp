#include "ble_client.hpp"
#include "diag_log.hpp"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include <vin_utils.h>

// These BLE headers re-introduce NimBLE's min()/max() macros (see ble_client.hpp);
// drop them again before the std::min use further down.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

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

static void scan_timeout_cb(void* arg) {
    static_cast<BleClient*>(arg)->on_scan_timeout();
}

BleClient::BleClient() {
    g_instance = this;
    write_mutex_  = xSemaphoreCreateMutex();
    scan_mutex_   = xSemaphoreCreateMutex();
    client_mutex_ = xSemaphoreCreateMutex();
    esp_timer_create_args_t ta{};
    ta.callback = scan_timeout_cb;
    ta.arg      = this;
    ta.name     = "ble_scan";
    esp_timer_create(&ta, &scan_timer_);
}

// Start a time-limited discovery scan (lists nearby Teslas, does not connect).
void BleClient::start_discovery(int ms) {
    if (is_connected() || connecting_) return;
    want_connect_ = false;
    if (!scanning_) start_scan_();
    if (scan_timer_) {
        esp_timer_stop(scan_timer_);
        esp_timer_start_once(scan_timer_, (int64_t)ms * 1000);
    }
    ESP_LOGI(TAG, "discovery scan started for %d ms", ms);
}

void BleClient::on_scan_timeout() {
    // Only end a pure discovery scan — never abort an in-flight connect attempt.
    if (scanning_ && !want_connect_ && !connecting_ && !is_connected()) {
        ble_gap_disc_cancel();
        scanning_ = false;
        ESP_LOGI(TAG, "discovery scan window ended");
    }
}

bool BleClient::start() {
    nimble_port_init();

    ble_hs_cfg.sync_cb  = on_sync_cb;
    ble_hs_cfg.reset_cb = on_reset_cb;

    // Prefer larger MTU to reduce fragmentation
    ble_att_set_preferred_mtu(247);

    // This device is BLE *central only* (it scans, connects, writes, subscribes).
    // It never advertises or exposes a GATT server, so the GAP/GATT *server*
    // services (ble_svc_gap/ble_svc_gatt) and the local device name are
    // intentionally not initialised. ESP-IDF 5.5 no longer compiles those
    // service sources when the peripheral role is disabled, so referencing them
    // would fail to link.

    nimble_port_freertos_init(nimble_host_task);
    return true;
}

void BleClient::on_sync() {
    ESP_LOGI(TAG, "NimBLE synced");
    // Idle: radio quiet. Discovery scanning is started manually for a limited window
    // (start_discovery), and a connect scan is started on demand by connect().
}

void BleClient::start_scan_() {
    ble_gap_disc_params params{};
    params.passive         = 0;
    // No duplicate filtering: we want repeated adverts so the listed RSSI stays fresh.
    params.filter_duplicates = 0;
    params.itvl            = 0x0010; // 10ms
    params.window          = 0x0010;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                           &params, gap_event_cb, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
        scanning_ = false;
    } else {
        scanning_ = true;
        ESP_LOGI(TAG, "scanning for Tesla BLE...");
    }
}

// Start a discovery scan if we are idle (not connected and not mid-connect).
void BleClient::ensure_scanning_() {
    if (is_connected() || connecting_ || scanning_) return;
    start_scan_();
}

// Upsert a discovered Tesla into the nearby list (called from the host task).
void BleClient::note_scan_(const ble_gap_disc_desc& d, const ble_hs_adv_fields& f) {
    if (!scan_mutex_) return;
    if (xSemaphoreTake(scan_mutex_, 0) != pdTRUE) return;  // never block the host task
    ScanEntry* e = nullptr;
    for (auto& s : scan_) {
        if (memcmp(s.addr, d.addr.val, 6) == 0) { e = &s; break; }
    }
    if (!e) {
        if (scan_.size() < 12) {
            scan_.push_back(ScanEntry{});
            e = &scan_.back();
            memcpy(e->addr, d.addr.val, 6);
            e->name[0] = '\0';
        } else {
            // Replace the stalest entry.
            e = &scan_[0];
            for (auto& s : scan_) if (s.last_us < e->last_us) e = &s;
            memcpy(e->addr, d.addr.val, 6);
            e->name[0] = '\0';
        }
    }
    e->rssi    = d.rssi;
    e->last_us = esp_timer_get_time();
    if (f.name != nullptr && f.name_len > 0) {
        size_t n = f.name_len < sizeof(e->name) - 1 ? f.name_len : sizeof(e->name) - 1;
        memcpy(e->name, f.name, n);
        e->name[n] = '\0';
    }
    xSemaphoreGive(scan_mutex_);
}

std::vector<TeslaScan> BleClient::nearby() const {
    std::vector<TeslaScan> out;
    if (!scan_mutex_) return out;
    const int64_t now = esp_timer_get_time();
    if (xSemaphoreTake(scan_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) return out;
    for (const auto& s : scan_) {
        if (now - s.last_us > 15LL * 1000 * 1000) continue;  // drop entries older than 15s
        char addr[18];
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 s.addr[5], s.addr[4], s.addr[3], s.addr[2], s.addr[1], s.addr[0]);
        out.push_back(TeslaScan{addr, s.name, s.rssi});
    }
    xSemaphoreGive(scan_mutex_);
    std::sort(out.begin(), out.end(),
              [](const TeslaScan& a, const TeslaScan& b) { return a.rssi > b.rssi; });
    return out;
}

std::string BleClient::peer_addr_str() const {
    if (!client_mutex_) return "";
    xSemaphoreTake(client_mutex_, portMAX_DELAY);
    std::string s = peer_addr_str_;
    xSemaphoreGive(client_mutex_);
    return s;
}

bool BleClient::connected_rssi(int8_t& out) const {
    if (!is_connected()) return false;
    return ble_gap_conn_rssi(conn_handle_, &out) == 0;
}

// BleAdapter::connect — called by Vehicle when it wants us to connect.
// Sets a connect intent; the running discovery scan connects to the next Tesla
// advert it sees. (The address argument is unused: we connect to the first Tesla
// found, robust to Tesla's rotating BLE addresses; correctness is enforced by the
// VIN/session layer.)
void BleClient::connect(const std::string& address) {
    (void)address;
    if (is_connected()) return;
    want_connect_ = true;
    ensure_scanning_();
}

// Drop a pending connect intent and return to idle scanning/listing.
void BleClient::stop_connecting() {
    want_connect_ = false;
    // No idle scanning: cancel the connect scan if it is still running.
    if (scanning_ && !is_connected()) {
        ble_gap_disc_cancel();
        scanning_ = false;
    }
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

        // Tesla vehicles advertise by NAME ("S<hex>C", derived from the VIN) — the
        // 128-bit service UUID is NOT in the advertisement/scan-response, so we match
        // on the name (carried in the scan response). The service UUID is only used
        // later for GATT discovery once connected.
        if (fields.name == nullptr || fields.name_len == 0) break;
        std::string adv_name((const char*)fields.name, fields.name_len);
        if (!TeslaBLE::is_tesla_vehicle_name(adv_name)) break;

        // Always record the Tesla in the nearby list (with RSSI) for the web UI.
        note_scan_(event->disc, fields);

        // Only connect when a command set a connect intent — otherwise keep scanning
        // and listing.
        if (!want_connect_ || connecting_) break;

        // Connect to the configured VIN's vehicle (or any Tesla if no VIN is set).
        if (!target_vin_.empty() && !TeslaBLE::matches_vin(adv_name, target_vin_)) break;

        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 event->disc.addr.val[5], event->disc.addr.val[4],
                 event->disc.addr.val[3], event->disc.addr.val[2],
                 event->disc.addr.val[1], event->disc.addr.val[0]);
        ESP_LOGI(TAG, "Tesla '%s' found: %s — connecting", adv_name.c_str(), addr_str);
        if (client_mutex_) {
            xSemaphoreTake(client_mutex_, portMAX_DELAY);
            peer_addr_str_ = addr_str;
            xSemaphoreGive(client_mutex_);
        }

        connecting_   = true;
        want_connect_ = false;
        scanning_     = false;
        ble_gap_disc_cancel();
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC,
                             &event->disc.addr,
                             10000, nullptr,
                             gap_event_cb, this);
        if (rc != 0) {
            ESP_LOGE(TAG, "connect failed: %d", rc);
            connecting_ = false;
            ensure_scanning_();
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT: {
        connecting_ = false;
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connect error: %d", event->connect.status);
            if (on_connected_) on_connected_(false);
            // Keep the intent so an in-flight command retries within its timeout
            // window; ensure_connected_() clears it via stop_connecting() on timeout.
            want_connect_ = true;
            ensure_scanning_();
            break;
        }
        conn_handle_  = event->connect.conn_handle;
        want_connect_ = false;
        ESP_LOGI(TAG, "connected, handle=%d", conn_handle_);

        // Reset discovery state for this fresh connection.
        svc_start_handle_  = 0;
        svc_end_handle_    = 0;
        write_handle_      = 0;
        notify_val_handle_ = 0;

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
        connecting_        = false;
        want_connect_      = false;
        scanning_          = false;
        if (client_mutex_) {
            xSemaphoreTake(client_mutex_, portMAX_DELAY);
            peer_addr_str_.clear();
            xSemaphoreGive(client_mutex_);
        }
        if (on_connected_) on_connected_(false);
        // Stay idle (no auto-scan); discovery is manual, connect is on demand.
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle != notify_val_handle_) break;
        struct os_mbuf* om = event->notify_rx.om;
        if (!om) break;
        uint16_t pkt_len = OS_MBUF_PKTLEN(om);
        std::vector<uint8_t> buf(pkt_len);
        int rc = os_mbuf_copydata(om, 0, pkt_len, buf.data());
        if (rc == 0) {
            if (diag_verbose()) {
                char hex[3*64+1]; size_t n = std::min<size_t>(pkt_len, 64); size_t p = 0;
                for (size_t i = 0; i < n; i++) p += snprintf(hex+p, sizeof(hex)-p, "%02x ", buf[i]);
                ESP_LOGI(TAG, "RX notify len=%u: %s", pkt_len, hex);
            }
            if (on_rx_data_) on_rx_data_(buf);
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
    // Keep the FIRST valid Tesla service match. NimBLE may invoke this callback an
    // extra time with a sentinel range (0xFFFF-0xFFFF); ignore invalid ranges so they
    // don't clobber the real handles and leave char discovery searching an empty range.
    if (svc && svc_start_handle_ == 0 &&
        svc->start_handle != 0 && svc->start_handle != 0xFFFF &&
        svc->start_handle <= svc->end_handle &&
        ble_uuid_cmp(&svc->uuid.u, &TESLA_SVC_UUID.u) == 0) {
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
