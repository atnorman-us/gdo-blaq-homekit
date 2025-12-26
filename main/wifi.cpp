#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_wifi_connect.h>

static const char* TAG = "wifi";

void app_wifi_init(void) {
    if (nvs_wifi_connect() != ESP_OK) {
        nvs_wifi_connect_start_http_server(NVS_WIFI_CONNECT_MODE_RESTART_ESP32, nullptr);
        return;
    }
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_STA) {
        ESP_LOGI(TAG, "Connected in STA mode");
    } else if (mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Running in AP mode");
        nvs_wifi_connect_start_http_server(NVS_WIFI_CONNECT_MODE_RESTART_ESP32, nullptr);
    } else {
        ESP_LOGI(TAG, "WiFi not configured");
    }
}

