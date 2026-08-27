#pragma once

#include <adapters.h>
#include <functional>
#include <array>
#include <vector>
#include <string>
#include <cstdint>
#include <atomic>
#include <esp_timer.h>

#include "logic/ble_readiness.hpp"
#include "logic/nimble_start_gate.hpp"

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
// Client Characteristic Configuration Descriptor — the standard 0x2902 descriptor written to
// enable notifications. Discovered at runtime rather than assumed at notify_val_handle_+1.
static const ble_uuid16_t CCCD_UUID = BLE_UUID16_INIT(0x2902);

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

    bool is_connected() const {
        if (disconnecting_.load()) return false;
        const uint32_t generation_before = connection_generation_.load();
        const uint16_t conn_handle = conn_handle_.load();
        const uint16_t write_handle = write_handle_.load();
        const uint32_t ready_generation = ready_generation_.load();
        const uint32_t generation_after = connection_generation_.load();
        return tk::ble::command_ready(disconnecting_.load(), generation_before,
                                      generation_after,
                                      conn_handle != BLE_HS_CONN_HANDLE_NONE,
                                      write_handle != 0, ready_generation);
    }
    std::string peer_addr_str() const;

    // RSSI (dBm) of the active connection; false if not connected / unavailable.
    bool connected_rssi(int8_t& out) const;

    // Best-known advert RSSI of the target, valid even while NOT connected: it's seeded from
    // the advert on every connect attempt and the failed-connect path doesn't clear it (only a
    // real link-drop does). Lets the web UI show real bars + dBm in the "can't connect" state
    // instead of empty bars. false if nothing seen / the link genuinely dropped.
    bool last_advert_rssi(int8_t& out) const {
        if (!conn_rssi_valid_.load()) return false;
        out = conn_rssi_.load();
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
    bool host_synced() const noexcept { return host_synced_.load(std::memory_order_acquire); }
    std::uint32_t host_reset_count() const noexcept {
        return host_reset_count_.load(std::memory_order_acquire);
    }

    // Consecutive failed connects to the *target* car — its advert was heard and the
    // VIN-derived name matched, but ble_gap_connect timed out/errored so the link never came
    // up (e.g. another device is already holding the car's BLE connection). Reset on a
    // successful connect. Returns 0 once attempts stop (>90 s since the last one) so a car
    // that simply drove off reads as "out of range", not "failing". The web UI uses this to
    // tell "found the car but can't connect" apart from "looking for the car".
    uint32_t connect_fail_recent() const;

    // Connectability of the *target* car's advert during the last 90 s, mirroring how Tesla's official
    // vehicle-command derives ErrMaxConnectionsExceeded (it keys off the scan result's
    // Connectable flag, NOT the connect error — a vehicle at its BLE connection limit
    // advertises non-connectable). -1 = target not seen recently / not yet known,
    // 0 = advertising NON-connectable (≈ at its ~3-device BLE limit), 1 = connectable. This
    // intentionally stabilized snapshot is for UI/status only; failure logs use the method below.
    int target_connectable() const;
    // Same verdict, but only when both the target name and the primary advert's connectability
    // bit were observed at/after `since_us`. Connect attempts use this per-attempt view so the
    // UI's deliberately stable 90 s cache (or a fresh SCAN_RSP carrying a stale bit) cannot
    // mislabel a car that has since left as a current GATT failure.
    int target_connectable_since(int64_t since_us) const;

    // Called from NimBLE host task callbacks — not for external use
    void on_sync();
    void on_reset();
    int  on_gap_event(ble_gap_event* event);
    int  on_svc_disc(uint16_t conn_handle, uint32_t generation,
                     const ble_gatt_error* error, const ble_gatt_svc* svc);
    int  on_chr_disc(uint16_t conn_handle, uint32_t generation,
                     const ble_gatt_error* error, const ble_gatt_chr* chr);
    int  on_dsc_disc(uint16_t conn_handle, const ble_gatt_error* error,
                     uint16_t chr_val_handle, const ble_gatt_dsc* dsc,
                     uint32_t generation);
    int  on_subscribe_write(uint16_t conn_handle, const ble_gatt_error* error,
                            uint32_t generation);

private:
    // All discovery-procedure state transitions are serialized by intent_mutex_. The locked
    // variants must only be called while holding it; the public wrapper acquires it for callers
    // such as on_sync and GAP retry callbacks.
    bool start_scan_locked_();
    bool cancel_scan_locked_();
    void ensure_scanning_locked_();
    void ensure_scanning_();
    // Terminate the currently published GAP link after the caller has already invalidated
    // readiness under intent_mutex_. Deliberately preserves a newer connect intent so the
    // delayed DISCONNECT event can restart that request.
    void terminate_published_link_();
    void note_scan_(const ble_gap_disc_desc& d, const ble_hs_adv_fields& f);
    // Cache connectability by address — primary adverts carry no name (Tesla puts the name in
    // the SCAN_RSP, but connectability only on the primary advert), so the first observation is
    // retained and copied into the identified Tesla entry once the name arrives.
    void note_connectable_(const ble_addr_t& addr, bool connectable);
    void subscribe_notify_(uint16_t conn_handle, uint32_t generation);
    bool has_gap_link_() const;
    bool connection_snapshot_matches_(uint16_t conn_handle, uint32_t generation) const;
    bool write_chunk_(uint16_t conn_handle, uint16_t write_handle,
                      const uint8_t* data, size_t len);

    // Discovery: nearby Teslas seen while not connected, and the connect intent.
    struct ScanEntry { uint8_t addr[6]; char name[24]; int8_t rssi; int64_t last_us;
                       int64_t connectable_us; bool connectable; };
    // `last_us` timestamps the name/RSSI report; `connectable_us` separately timestamps the
    // primary advert that supplied `connectable`. SCAN_RSP must never make an older/default
    // connectability verdict look current.
    std::vector<ScanEntry> scan_;
    struct ConnectableObservation {
        uint8_t addr[6];
        int64_t seen_us;
        bool connectable;
    };
    // Primary adverts are normally nameless and precede the SCAN_RSP that identifies a Tesla.
    // Preserve their bit in a fixed, allocation-free host-task cache so the first observation of
    // a new/rotating address can be correlated once its name arrives.
    std::array<ConnectableObservation, 12> connectable_observations_{};
    SemaphoreHandle_t      scan_mutex_{nullptr};
    esp_timer_handle_t     scan_timer_{nullptr};
    // Serializes manual discovery, connect cancellation, GAP publication and CCCD readiness.
    // Atomics make individual fields race-free; this mutex supplies the multi-step linearization
    // point so a stale callback cannot publish on_connected(true) after a deadline cancellation.
    SemaphoreHandle_t      intent_mutex_{nullptr};
    // atomic (not volatile): each is written on the NimBLE host task and read from the
    // command / status / auto-pair tasks. volatile blocks some compiler optimizations but
    // is not a happens-before edge under the C++ memory model; std::atomic is (simple
    // seq_cst). Each is an independent single-value flag, so an atomic each is correct
    // (no multi-field invariant is being split).
    std::atomic<bool>      want_connect_{false};
    std::atomic<bool>      connecting_{false};
    std::atomic<bool>      scanning_{false};
    // True only after the NimBLE host has signalled sync (on_sync). Until then ANY
    // ble_gap_* call hits an uninitialised host — a benign error on ESP-IDF 5.4 but a
    // null-deref crash (LoadProhibited) on 5.5. ble_client.start() runs only after WiFi
    // association (~4 s), which can lose the race with auto_pair's fixed 4 s warm-up, so
    // gate the scan on the real host state rather than on timing.
    std::atomic<bool>      host_synced_{false};
    // Monotonic evidence that the essential host has reset since boot. Current synced=true alone
    // would hide a reset followed by a quick re-sync between health samples and could spend OTA
    // rollback on an unstable image.
    std::atomic<std::uint32_t> host_reset_count_{0};
    // nimble_port_freertos_init() is a void, unchecked wrapper in the pinned IDF. Only the first
    // on_sync callback proves that its hidden task allocation succeeded. This separate one-shot
    // gate makes a timeout terminal even if a callback arrives after boot has failed closed.
    tk::NimbleStartGate    start_gate_;

    ConnectedCb on_connected_;
    RxDataCb    on_rx_data_;

    // GAP/GATT handles are published by the NimBLE host task and consumed by command/status
    // tasks. Plain uint16_t fields made every is_connected/disconnect/write overlap a C++ data
    // race. Atomics make each snapshot defined; connection_generation_ is a tiny seqlock:
    // odd while the host publishes/invalidates handles, even while stable. It additionally
    // prevents a multi-chunk write from continuing on a replacement connection.
    std::atomic<uint16_t> conn_handle_{BLE_HS_CONN_HANDLE_NONE};
    std::atomic<uint32_t> connection_generation_{0};
    // A GAP handle is not command-ready. This token is published only after Tesla GATT
    // discovery, a confirmed CCCD write, and on_connected_(true) for the same generation.
    // Tying it to the generation also rejects a delayed callback after handle reuse.
    std::atomic<uint32_t> ready_generation_{tk::ble::kNoReadyGeneration};
    // disconnect() is asynchronous: the GAP event that clears the handle arrives later. This
    // flag closes that interval immediately, so a following command cannot enqueue on a link
    // already being terminated. The host task clears it only after publishing a fresh link.
    std::atomic<bool> disconnecting_{false};
    // Connect-failure tracking for the target car (see connect_fail_recent()). Stamped on
    // every connect attempt; the count climbs on each GAP connect error and resets on a
    // successful link. Written on the NimBLE host task, read by the status reader — atomic
    // so the reader sees a defined value (the counter's ++ is also naturally atomic now).
    std::atomic<uint32_t> connect_fail_count_{0};
    std::atomic<int64_t>  last_connect_attempt_us_{0};
    std::atomic<uint16_t> write_handle_{0};
    uint16_t notify_handle_{0};
    uint16_t notify_val_handle_{0};
    uint16_t cccd_handle_{0};   // discovered CCCD (0x2902) for the notify chr; 0 until found

    // Last-known link RSSI: seeded from the advert we connected to, then refreshed by
    // every successful live read in connected_rssi(). The live HCI "Read RSSI" can fail
    // transiently (e.g. while the controller is busy pairing), so this fallback keeps the
    // web UI showing real signal strength during pairing instead of nothing. mutable: the
    // refresh happens inside the const connected_rssi() accessor.
    mutable std::atomic<int8_t> conn_rssi_{0};
    mutable std::atomic<bool>   conn_rssi_valid_{false};

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
// Current host health, not the one-shot startup acknowledgement. OTA rollback may be committed
// only while this is true; an on_reset without a later on_sync is degraded runtime, not health.
bool ble_host_synced() noexcept;
std::uint32_t ble_host_reset_count() noexcept;
