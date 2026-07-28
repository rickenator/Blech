#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "bt_manager.h"
#include "chat_service.h"
#include "https_server.h"
#include "settings_store.h"
#include "status_led.h"
#include "wifi_manager.h"
#include "serial_repl.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(settings_store_init());

    status_led_init();

    ESP_ERROR_CHECK(chat_service_start());

    err = wifi_manager_start();
    serial_repl_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        status_led_set(STATUS_LED_ERROR);
    }

    err = https_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS start failed: %s", esp_err_to_name(err));
        status_led_set(STATUS_LED_ERROR);
    }

    err = bt_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Blech ready; backend=%s",
             chat_service_model_ready() ? "ready" : "needs Wi-Fi");
}
