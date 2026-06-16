#include <cstring>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "ble_client.hpp"
#include "nvs_storage.hpp"
#include "vehicle_ctrl.hpp"
#include "http_server.hpp"

static const char* TAG = "main";

// ─── WiFi ─────────────────────────────────────────────────────────────────────

static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT      = BIT1;
static int s_retry_num              = 0;
static const int MAX_RETRY          = 10;

static void wifi_event_handler(void*, esp_event_base_t base,
                                int32_t event_id, void* data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi retry %d/%d", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(const char* ssid, const char* password) {
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, &h2));

    wifi_config_t wifi_cfg{};
    strncpy((char*)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char*)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected to '%s'", ssid);
        return true;
    }
    ESP_LOGE(TAG, "WiFi connection failed");
    return false;
}

// ─── app_main ─────────────────────────────────────────────────────────────────

extern "C" void app_main() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Static so they outlive app_main() (which deletes itself via vTaskDelete)
    static NvsStorageAdapter config_store("tesla_cfg");
    config_store.initialize();

    // Resolve WiFi credentials: NVS overrides Kconfig defaults
    static std::string ssid     = CONFIG_TESLA_WIFI_SSID;
    static std::string password = CONFIG_TESLA_WIFI_PASSWORD;
    config_store.load_str("wifi_ssid", ssid);
    config_store.load_str("wifi_pass", password);

    if (ssid.empty()) {
        ESP_LOGE(TAG, "WiFi SSID not configured. Set CONFIG_TESLA_WIFI_SSID via menuconfig.");
        while (true) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // Resolve VIN
    static std::string vin = CONFIG_TESLA_VIN;
    config_store.load_str("vin", vin);
    if (vin.empty()) {
        ESP_LOGW(TAG, "VIN not configured — using 'UNKNOWN'. Set CONFIG_TESLA_VIN or NVS key 'vin'.");
        vin = "UNKNOWN";
    }

    // Resolve BLE MAC (persisted after first successful scan)
    static std::string ble_mac = CONFIG_TESLA_BLE_MAC;
    config_store.load_str("ble_mac", ble_mac);

    ESP_LOGI(TAG, "VIN: %s  BLE MAC: %s", vin.c_str(),
             ble_mac.empty() ? "(scan)" : ble_mac.c_str());

    // Connect to WiFi
    if (!wifi_connect(ssid.c_str(), password.c_str())) {
        ESP_LOGE(TAG, "WiFi failed — rebooting in 10s");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    // BLE + NVS storage for Tesla sessions/key
    static NvsStorageAdapter tesla_store("tesla_ble");
    tesla_store.initialize();

    static BleClient ble_client;

    static VehicleController vehicle;
    // init() wires the connected + rx callbacks onto ble_client.
    // It also passes the config_store so it can save the discovered MAC.
    vehicle.init(vin, ble_client, tesla_store, config_store, ble_mac);

    // Start NimBLE host; if MAC is known it connects directly, else scans.
    if (!ble_mac.empty()) {
        ble_client.connect(ble_mac);
    }
    ble_client.start();

    http_server_start(vehicle);

    ESP_LOGI(TAG, "esp32-tesla-key running. API on port 80.");
    // Main task is no longer needed; Vehicle loop + HTTP server run in their own tasks.
    vTaskDelete(nullptr);
}
