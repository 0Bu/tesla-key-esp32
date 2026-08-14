#include "ble_client.hpp"
#include "diag_log.hpp"
#include "rtos_guard.hpp"
#include <esp_log.h>
#include <esp_timer.h>
#include <array>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <exception>

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
    if (g_instance) g_instance->on_reset();
}

static int gap_event_cb(ble_gap_event* event, void* arg) {
    auto* client = static_cast<BleClient*>(arg);
    return client->on_gap_event(event);
}

static int svc_disc_cb(uint16_t conn_handle, const ble_gatt_error* error,
                       const ble_gatt_svc* svc, void* arg) {
    // The callback argument is an opaque connection-generation token, not an object pointer.
    // BleClient is already a singleton in this adapter; carrying the token through each async
    // GATT procedure lets a delayed callback be rejected even when NimBLE rapidly reuses the
    // same 16-bit connection handle for a replacement link.
    auto* client = ble_client_instance();
    if (!client) return 0;
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    return client->on_svc_disc(conn_handle, generation, error, svc);
}

static int chr_disc_cb(uint16_t conn_handle, const ble_gatt_error* error,
                       const ble_gatt_chr* chr, void* arg) {
    auto* client = ble_client_instance();
    if (!client) return 0;
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    return client->on_chr_disc(conn_handle, generation, error, chr);
}

static int dsc_disc_cb(uint16_t conn_handle, const ble_gatt_error* error,
                       uint16_t chr_val_handle, const ble_gatt_dsc* dsc, void* arg) {
    auto* client = ble_client_instance();
    if (!client) return 0;
    const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    return client->on_dsc_disc(conn_handle, error, chr_val_handle, dsc, generation);
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
    if (esp_timer_create(&ta, &scan_timer_) != ESP_OK) {
        ESP_LOGE(TAG, "failed to create BLE scan timer");
        scan_timer_ = nullptr;
    }
}

// Start a time-limited discovery scan (lists nearby Teslas, does not connect).
void BleClient::start_discovery(int ms) {
    if (disconnecting_.load() || is_connected() || connecting_) return;
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
    if (!write_mutex_ || !scan_mutex_ || !client_mutex_ || !scan_timer_) {
        ESP_LOGE(TAG, "BLE resource allocation failed");
        return false;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        // ESSENTIAL: with no NimBLE host there is no BLE proxy. Report failure so app_main can
        // halt boot rather than run a controller that can never connect (issue #204).
        ESP_LOGE(TAG, "nimble_port_init failed: %d", (int)err);
        return false;
    }

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
    host_synced_ = true;
    ESP_LOGI(TAG, "NimBLE synced");
    if (want_connect_.load()) ensure_scanning_();
    // Idle: radio quiet. Discovery scanning is started manually for a limited window
    // (start_discovery), and a connect scan is started on demand by connect().
}

void BleClient::on_reset() {
    disconnecting_.store(true);
    // Odd generation means "handles changing". Publish that before any other host-reset
    // bookkeeping so a command task cannot snapshot the old handles as a new stable link.
    connection_generation_.fetch_add(1);
    // Host went down; ble_gap_* calls are unsafe again until it re-syncs.
    host_synced_ = false;
    scanning_    = false;
    conn_handle_.store(BLE_HS_CONN_HANDLE_NONE);
    write_handle_.store(0);
    conn_rssi_valid_.store(false);
    disconnecting_.store(false);
    connection_generation_.fetch_add(1);  // even: disconnected snapshot is stable
    if (on_connected_) on_connected_(false);
}

void BleClient::start_scan_() {
    // The NimBLE host must have synced before any ble_gap_* call; before that the call
    // dereferences uninitialised host state — a benign error on ESP-IDF 5.4 but a
    // LoadProhibited crash on 5.5. Skip silently: ensure_scanning_() is retried by
    // auto_pair / loop / connect, so the scan starts as soon as the host is up.
    if (!host_synced_ || disconnecting_.load()) return;
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
    if (disconnecting_.load() || is_connected() || connecting_ || scanning_) return;
    start_scan_();
}

// Upsert a discovered Tesla into the nearby list (called from the host task).
void BleClient::note_scan_(const ble_gap_disc_desc& d, const ble_hs_adv_fields& f) {
    if (!scan_mutex_) return;
    // try-lock: never block the host task. RAII give — scan_.push_back() below can throw
    // bad_alloc on a fragmented heap, and a hand-rolled give would then be skipped.
    tk::SemGuard g(scan_mutex_, 0);
    if (!g) return;
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
            e->connectable = true;   // optimistic until a primary advert proves otherwise
        } else {
            // Replace the stalest entry.
            e = &scan_[0];
            for (auto& s : scan_) if (s.last_us < e->last_us) e = &s;
            memcpy(e->addr, d.addr.val, 6);
            e->name[0] = '\0';
            e->connectable = true;
        }
    }
    e->rssi    = d.rssi;
    e->last_us = esp_timer_get_time();
    // If THIS report is a primary advert (not a scan response), it tells us the connectability
    // directly. SCAN_RSP carries no connectability, so leave the prior value untouched.
    switch (d.event_type) {
        case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
        case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:     e->connectable = true;  break;
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
        case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND: e->connectable = false; break;
        default: break;  // SCAN_RSP — connectability unknown from this PDU
    }
    if (f.name != nullptr && f.name_len > 0) {
        size_t n = f.name_len < sizeof(e->name) - 1 ? f.name_len : sizeof(e->name) - 1;
        memcpy(e->name, f.name, n);
        e->name[n] = '\0';
    }
}

std::vector<TeslaScan> BleClient::nearby() const {
    std::vector<TeslaScan> out;
    if (!scan_mutex_) return out;
    const int64_t now = esp_timer_get_time();
    std::array<ScanEntry, 12> snapshot{};
    size_t count = 0;
    {
        // Copy only the fixed-size records under the host-task mutex. String allocations and
        // sorting happen after release, so a slow /scan response cannot starve advert updates.
        tk::SemGuard g(scan_mutex_, pdMS_TO_TICKS(50));
        if (!g) return out;
        count = std::min(snapshot.size(), scan_.size());
        std::copy_n(scan_.begin(), count, snapshot.begin());
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& s = snapshot[i];
        if (now - s.last_us > 15LL * 1000 * 1000) continue;  // drop entries older than 15s
        char addr[18];
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 s.addr[5], s.addr[4], s.addr[3], s.addr[2], s.addr[1], s.addr[0]);
        out.push_back(TeslaScan{addr, s.name, s.rssi, s.connectable});
    }
    std::sort(out.begin(), out.end(),
              [](const TeslaScan& a, const TeslaScan& b) { return a.rssi > b.rssi; });
    return out;
}

void BleClient::note_connectable_(const ble_addr_t& addr, bool connectable) {
    if (!scan_mutex_) return;
    tk::SemGuard g(scan_mutex_, 0);  // try-lock: never block the host task
    if (!g) return;
    // Only updates entries we already know are Teslas (created by note_scan_ on a name match);
    // a nameless primary advert for an unknown address is ignored.
    for (auto& s : scan_) {
        if (memcmp(s.addr, addr.val, 6) == 0) { s.connectable = connectable; break; }
    }
}

int BleClient::target_connectable() const {
    if (!scan_mutex_ || target_vin_.empty()) return -1;
    const int64_t now = esp_timer_get_time();
    int result = -1;
    std::array<ScanEntry, 12> snapshot{};
    size_t count = 0;
    {
        tk::SemGuard g(scan_mutex_, pdMS_TO_TICKS(50));
        if (!g) return -1;
        count = std::min(snapshot.size(), scan_.size());
        std::copy_n(scan_.begin(), count, snapshot.begin());
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& s = snapshot[i];
        // 90 s window: a PAIRED device only scans briefly around each ~30-40 s health probe, so
        // a 15 s freshness would flap to "unknown" between probes; 90 s keeps the at-limit signal
        // stable across the gap. ("at its BLE limit" is a persistent condition, so a slightly
        // older observation is still meaningful.)
        if (now - s.last_us > 90LL * 1000 * 1000) continue;  // stale
        if (s.name[0] == '\0') continue;
        if (TeslaBLE::matches_vin(std::string(s.name), target_vin_)) {
            result = s.connectable ? 1 : 0;
            break;
        }
    }
    return result;
}

std::string BleClient::peer_addr_str() const {
    if (!client_mutex_) return "";
    // RAII give — the string copy can throw bad_alloc; the guard releases on unwind.
    tk::SemGuard g(client_mutex_);
    return peer_addr_str_;
}

uint32_t BleClient::connect_fail_recent() const {
    int64_t last = last_connect_attempt_us_.load();
    if (last == 0) return 0;
    // Stale: no attempt in the last 90 s ⇒ the car is no longer in range / we stopped trying,
    // so this is "out of range", not "failing to connect". 90 s spans the slowest attempt cadence
    // — a PAIRED device retries only ~every 30-40 s via the health probe (vs ~10 s while bringing
    // up an unpaired one), so the signal stays stable across that gap. Resets the moment a
    // connect succeeds (connect_fail_count_ → 0).
    if (esp_timer_get_time() - last > 90LL * 1000 * 1000) return 0;
    return connect_fail_count_.load();
}

bool BleClient::connected_rssi(int8_t& out) const {
    if (disconnecting_.load()) return false;
    const uint32_t generation = connection_generation_.load();
    if (generation & 1U) return false;
    const uint16_t conn_handle = conn_handle_.load();
    if (disconnecting_.load() || conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        connection_generation_.load() != generation) return false;
    // Prefer a fresh live read; refresh the cache when it succeeds. 127 is NimBLE's
    // "RSSI unknown" sentinel, so treat it as a failed read.
    int8_t live = 0;
    if (ble_gap_conn_rssi(conn_handle, &live) == 0 && live != 127) {
        if (disconnecting_.load() || connection_generation_.load() != generation) return false;
        conn_rssi_.store(live);
        conn_rssi_valid_.store(true);
        if (disconnecting_.load() || connection_generation_.load() != generation) {
            // Disconnect may have cleared the cache between the check and the stores above.
            // Never let this old-link sample resurrect as the next link's fallback RSSI.
            conn_rssi_valid_.store(false);
            return false;
        }
        out = live;
        return true;
    }
    // Live read failed (common while the controller is busy pairing) — fall back to the
    // last-known value (seeded from the connect-time advert) so the UI still shows signal.
    if (!disconnecting_.load() && connection_generation_.load() == generation &&
        conn_rssi_valid_.load()) {
        out = conn_rssi_.load();
        return true;
    }
    return false;
}

// BleAdapter::connect — called by Vehicle when it wants us to connect.
// Sets a connect intent; the running discovery scan connects to the next advert matching the
// configured target VIN. (The address argument is unused: we match by the VIN-derived name,
// robust to Tesla's rotating BLE addresses; correctness is enforced by the VIN/session layer.
// With no target VIN configured the scan still lists but never connects — see on_gap_event.)
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
    // ble_gap_terminate is asynchronous. Make the software link unavailable before issuing it,
    // otherwise a following request can reuse the doomed handle until GAP_DISCONNECT arrives.
    disconnecting_.store(true);
    want_connect_.store(false);
    // A connect/disconnect host callback changes the seqlock only for a very short publish
    // interval. If this request lands inside it, yield until a stable snapshot exists rather
    // than losing the termination request and letting that just-published link escape.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t generation = connection_generation_.load();
        if (generation & 1U) {
            taskYIELD();
            continue;
        }
        const uint16_t conn_handle = conn_handle_.load();
        if (connection_generation_.load() != generation) continue;
        // The connect publisher clears this flag before its final even generation. Reasserting
        // it after the stable snapshot therefore cannot be overwritten by that same publish.
        disconnecting_.store(true);
        if (connection_generation_.load() != generation) continue;
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            disconnecting_.store(false);
        }
        return;
    }
    ESP_LOGW(TAG, "could not obtain stable BLE handle while disconnecting");
}

bool BleClient::write(const std::vector<uint8_t>& data) {
    tk::SemGuard g(write_mutex_, pdMS_TO_TICKS(500));
    if (!g) return false;

    const uint32_t generation = connection_generation_.load();
    if (disconnecting_.load() || (generation & 1U)) return false;
    const uint16_t conn_handle = conn_handle_.load();
    const uint16_t write_handle = write_handle_.load();
    if (disconnecting_.load() || conn_handle == BLE_HS_CONN_HANDLE_NONE || write_handle == 0 ||
        connection_generation_.load() != generation) return false;

    for (size_t offset = 0; offset < data.size(); offset += BLE_CHUNK_SIZE) {
        if (disconnecting_.load() || connection_generation_.load() != generation) return false;
        size_t chunk = std::min(BLE_CHUNK_SIZE, data.size() - offset);
        if (!write_chunk_(conn_handle, write_handle, data.data() + offset, chunk)) {
            return false;
        }
        // A GAP disconnect can race the no-response GATT call itself. Checking only before
        // each chunk left the final chunk reporting success even though the host invalidated
        // that connection while it was being submitted.
        if (disconnecting_.load() || connection_generation_.load() != generation) return false;
        if (offset + chunk < data.size()) {
            vTaskDelay(pdMS_TO_TICKS(10)); // small gap between chunks
        }
    }

    return true;
}

bool BleClient::write_chunk_(uint16_t conn_handle, uint16_t write_handle,
                             const uint8_t* data, size_t len) {
    int rc = ble_gattc_write_no_rsp_flat(conn_handle, write_handle, data, len);
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE write chunk failed: %d", rc);
        return false;
    }
    return true;
}

// ─── GAP event handler ────────────────────────────────────────────────────────

int BleClient::on_gap_event(ble_gap_event* event) {
    // Runs on the NimBLE host task (dispatched from the C gap_event_cb, no try/catch in the
    // chain). The NOTIFY_RX and DISC cases allocate from the heap, so an OOM std::bad_alloc on
    // a fragmented heap would unwind into C frames → std::terminate → abort → reboot (and a
    // reboot loop also re-opens the poll window, defeating car-sleep). Contain it here — drop
    // the event — mirroring the guards in vehicle_ctrl (on_rx_data) and the HTTP handler.
    try {
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        // Check if this device advertises Tesla service UUID
        ble_hs_adv_fields fields{};
        int rc = ble_hs_adv_parse_fields(&fields,
                                          event->disc.data,
                                          event->disc.length_data);
        if (rc != 0) break;

        // Record connectability from the PRIMARY advert. Tesla carries the vehicle name only
        // in the SCAN_RSP (handled by note_scan_ below), but connectability lives on the
        // primary advert — which often arrives nameless and would otherwise break out here. A
        // car at its BLE connection limit advertises non-connectable (this is exactly the
        // signal vehicle-command's ErrMaxConnectionsExceeded keys off). note_connectable_ only
        // touches addresses already known to be Teslas, so it's a no-op for everything else.
        switch (event->disc.event_type) {
            case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
            case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:     note_connectable_(event->disc.addr, true);  break;
            case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
            case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND: note_connectable_(event->disc.addr, false); break;
            default: break;  // SCAN_RSP
        }

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

        // Connect ONLY to the configured VIN's vehicle. With no VIN configured the target is
        // empty and we never connect/enrol — the device enrols a Charging-Manager key and must
        // not pair onto an arbitrary nearby Tesla. (Listing via note_scan_ above still works.)
        if (target_vin_.empty() || !TeslaBLE::matches_vin(adv_name, target_vin_)) break;

        char addr_str[18];
        snprintf(addr_str, sizeof(addr_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 event->disc.addr.val[5], event->disc.addr.val[4],
                 event->disc.addr.val[3], event->disc.addr.val[2],
                 event->disc.addr.val[1], event->disc.addr.val[0]);
        ESP_LOGI(TAG, "Tesla '%s' found: %s — connecting", adv_name.c_str(), addr_str);
        {
            tk::SemGuard g(client_mutex_);   // RAII: peer_addr_str_ = … can throw
            if (g) peer_addr_str_ = addr_str;
        }
        // Seed the link RSSI from this advert so the UI has a real value to show from the
        // moment we connect (incl. while pairing), before the first live read succeeds.
        conn_rssi_.store(event->disc.rssi);
        conn_rssi_valid_.store(true);

        connecting_   = true;
        want_connect_ = false;
        scanning_     = false;
        ble_gap_disc_cancel();
        last_connect_attempt_us_.store(esp_timer_get_time()); // marks the link as "actively trying"
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC,
                             &event->disc.addr,
                             10000, nullptr,
                             gap_event_cb, this);
        if (rc != 0) {
            ESP_LOGE(TAG, "connect failed: %d", rc);
            connect_fail_count_.fetch_add(1);
            connecting_ = false;
            ensure_scanning_();
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT: {
        connecting_ = false;
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connect error: %d", event->connect.status);
            connect_fail_count_.fetch_add(1);   // advert was heard but the link never came up
            if (on_connected_) on_connected_(false);
            // Keep the intent so an in-flight command retries within its timeout
            // window; ensure_connected_() clears it via stop_connecting() on timeout.
            want_connect_ = true;
            ensure_scanning_();
            break;
        }
        // Publish a fresh, internally consistent handle generation. Odd means the host is
        // changing the snapshot; command tasks reject it until the final even generation.
        connection_generation_.fetch_add(1);
        write_handle_.store(0);
        conn_handle_.store(event->connect.conn_handle);
        disconnecting_.store(false);
        connection_generation_.fetch_add(1);
        want_connect_ = false;
        connect_fail_count_.store(0);   // link is up — clear the "can't connect" signal
        ESP_LOGI(TAG, "connected, handle=%d", event->connect.conn_handle);

        // Reset discovery state for this fresh connection.
        svc_start_handle_  = 0;
        svc_end_handle_    = 0;
        write_handle_.store(0);
        notify_val_handle_ = 0;
        cccd_handle_       = 0;

        // Discover Tesla service
        const uint32_t generation = connection_generation_.load();
        void* generation_arg = reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
        int rc = ble_gattc_disc_svc_by_uuid(event->connect.conn_handle,
                                             &TESLA_SVC_UUID.u,
                                             svc_disc_cb, generation_arg);
        if (rc != 0) {
            ESP_LOGE(TAG, "svc discovery failed: %d", rc);
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        // Invalidate first: set_connected(false) and command tasks can run synchronously from
        // the callbacks below, and none may adopt the old handles under a fresh generation.
        disconnecting_.store(true);
        connection_generation_.fetch_add(1);
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        conn_handle_.store(BLE_HS_CONN_HANDLE_NONE);
        write_handle_.store(0);
        notify_handle_     = 0;
        notify_val_handle_ = 0;
        cccd_handle_       = 0;
        connecting_        = false;
        // A new command may have called connect() after disconnect() initiated termination but
        // before this delayed event arrived. Preserve that fresh intent and restart its scan
        // after the old link snapshot has been retired.
        const bool reconnect = want_connect_.load();
        scanning_          = false;
        conn_rssi_valid_.store(false);   // stale once the link is gone
        {
            tk::SemGuard g(client_mutex_);   // RAII give
            if (g) peer_addr_str_.clear();
        }
        disconnecting_.store(false);
        connection_generation_.fetch_add(1);  // even: disconnected snapshot is stable
        if (on_connected_) on_connected_(false);
        if (reconnect) ensure_scanning_();
        // Otherwise stay idle (no auto-scan); discovery is manual, connect is on demand.
        break;
    }

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
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "on_gap_event exception (dropping event type=%d): %s",
                 event->type, e.what());
    } catch (...) {
        ESP_LOGE(TAG, "on_gap_event unknown exception (dropping event type=%d)", event->type);
    }
    return 0;
}

// ─── GATT service discovery ───────────────────────────────────────────────────

// The three discovery callbacks below are NimBLE-host-task entry points just like
// on_gap_event (dispatched from C, no try/catch in the chain) — on_dsc_disc in particular
// ends in on_connected_(true), whose vehicle_ctrl lambda allocates (std::string, NVS).
// An escaping std::bad_alloc would unwind into C frames → std::terminate → reboot, and a
// reboot loop re-opens the poll window, defeating car-sleep. Contain it per callback; a
// caught throw aborts this connection attempt cleanly (disconnect), the next on-demand
// connect retries discovery from scratch.

bool BleClient::connection_snapshot_matches_(uint16_t conn_handle,
                                              uint32_t generation) const {
    if ((generation & 1U) || disconnecting_.load() ||
        connection_generation_.load() != generation) return false;
    const uint16_t current = conn_handle_.load();
    return current == conn_handle && !disconnecting_.load() &&
           connection_generation_.load() == generation;
}

int BleClient::on_svc_disc(uint16_t conn_handle, uint32_t generation,
                            const ble_gatt_error* error,
                            const ble_gatt_svc* svc) {
    if (!connection_snapshot_matches_(conn_handle, generation)) return 0;
    try {
    if (error->status == BLE_HS_EDONE) {
        if (svc_start_handle_ == 0) {
            ESP_LOGE(TAG, "Tesla service not found");
            disconnect();
            return 0;
        }
        // Service found — discover characteristics
        void* generation_arg = reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
        int rc = ble_gattc_disc_all_chrs(conn_handle,
                                          svc_start_handle_, svc_end_handle_,
                                          chr_disc_cb, generation_arg);
        if (rc != 0) {
            ESP_LOGE(TAG, "characteristic discovery start failed: %d", rc);
            disconnect();
        }
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
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "on_svc_disc exception (dropping connection): %s", e.what());
        disconnect();
    } catch (...) {
        ESP_LOGE(TAG, "on_svc_disc unknown exception (dropping connection)");
        disconnect();
    }
    return 0;
}

int BleClient::on_chr_disc(uint16_t conn_handle, uint32_t generation,
                            const ble_gatt_error* error,
                            const ble_gatt_chr* chr) {
    if (!connection_snapshot_matches_(conn_handle, generation)) return 0;
    try {
    if (error->status == BLE_HS_EDONE) {
        const uint16_t write_handle = write_handle_.load();
        if (write_handle == 0 || notify_val_handle_ == 0) {
            ESP_LOGE(TAG, "required characteristics not found (write=%d notify=%d)",
                     write_handle, notify_val_handle_);
            disconnect();
            return 0;
        }
        ESP_LOGI(TAG, "BLE ready (write=%d notify=%d)",
                 write_handle, notify_val_handle_);
        // Subscribe to notifications
        subscribe_notify_(conn_handle, generation);
        return 0;
    }
    if (error->status != 0 || !chr) return 0;

    if (ble_uuid_cmp(&chr->uuid.u, &TESLA_WRITE_UUID.u) == 0) {
        write_handle_.store(chr->val_handle);
        ESP_LOGD(TAG, "write chr: %d", chr->val_handle);
    } else if (ble_uuid_cmp(&chr->uuid.u, &TESLA_NOTIFY_UUID.u) == 0) {
        notify_handle_     = chr->def_handle;
        notify_val_handle_ = chr->val_handle;
        ESP_LOGD(TAG, "notify chr: val=%d", notify_val_handle_);
    }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "on_chr_disc exception (dropping connection): %s", e.what());
        disconnect();
    } catch (...) {
        ESP_LOGE(TAG, "on_chr_disc unknown exception (dropping connection)");
        disconnect();
    }
    return 0;
}

void BleClient::subscribe_notify_(uint16_t conn_handle, uint32_t generation) {
    // Discover the CCCD (Client Characteristic Configuration Descriptor, 0x2902) for the notify
    // characteristic instead of assuming it sits at notify_val_handle_ + 1. GATT does not
    // guarantee that layout — an extra descriptor (e.g. a Characteristic User Description) placed
    // between the value and the CCCD, which a future Tesla vehicle-firmware GATT revision could
    // introduce, would shift it. Writing the enable word to the wrong handle would silently break
    // the device's ONLY receive channel: notifications never arrive, so every command then times
    // out as "vehicle not reachable" with no other symptom. Discover, then enable in on_dsc_disc().
    if (!connection_snapshot_matches_(conn_handle, generation)) return;
    cccd_handle_ = 0;
    void* generation_arg = reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
    int rc = ble_gattc_disc_all_dscs(conn_handle, notify_val_handle_, svc_end_handle_,
                                     dsc_disc_cb, generation_arg);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCCD discovery start failed: %d", rc);
        disconnect();
    }
}

int BleClient::on_dsc_disc(uint16_t conn_handle, const ble_gatt_error* error,
                           uint16_t chr_val_handle, const ble_gatt_dsc* dsc,
                           uint32_t generation) {
    if (!connection_snapshot_matches_(conn_handle, generation) ||
        chr_val_handle != notify_val_handle_) return 0;
    try {
    if (error->status == BLE_HS_EDONE) {
        if (cccd_handle_ == 0) {
            ESP_LOGE(TAG, "CCCD (0x2902) not found for notify chr — cannot subscribe");
            disconnect();
            return 0;
        }
        uint8_t value[2] = {0x01, 0x00};   // 0x0001 = enable notifications (BLE_GATT_SUB_NOTIFY)
        int rc = ble_gattc_write_flat(conn_handle, cccd_handle_,
                                       value, sizeof(value), nullptr, nullptr);
        if (rc != 0) {
            ESP_LOGE(TAG, "subscribe notify failed: %d", rc);
            disconnect();
            return 0;
        }
        if (!connection_snapshot_matches_(conn_handle, generation)) return 0;
        ESP_LOGI(TAG, "subscribed to Tesla notifications (CCCD handle %d)", cccd_handle_);
        if (on_connected_) on_connected_(true);
        return 0;
    }
    if (error->status != 0 || !dsc) return 0;
    // First 0x2902 at/after the notify value handle is that characteristic's CCCD.
    if (cccd_handle_ == 0 && ble_uuid_cmp(&dsc->uuid.u, &CCCD_UUID.u) == 0) {
        cccd_handle_ = dsc->handle;
        ESP_LOGD(TAG, "found CCCD at handle %d", cccd_handle_);
    }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "on_dsc_disc exception (dropping connection): %s", e.what());
        disconnect();
    } catch (...) {
        ESP_LOGE(TAG, "on_dsc_disc unknown exception (dropping connection)");
        disconnect();
    }
    return 0;
}
