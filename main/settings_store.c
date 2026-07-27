#include "settings_store.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *NAMESPACE = "agent";
static SemaphoreHandle_t lock;
static app_settings_t current;

static void load_string(nvs_handle_t handle, const char *key, char *value,
                        size_t value_size)
{
    size_t required = value_size;
    if (nvs_get_str(handle, key, value, &required) != ESP_OK) {
        value[0] = '\0';
    }
}

esp_err_t settings_store_init(void)
{
    lock = xSemaphoreCreateMutex();
    if (!lock) {
        return ESP_ERR_NO_MEM;
    }

    memset(&current, 0, sizeof(current));
    strlcpy(current.ssid, CONFIG_CHAT_WIFI_SSID, sizeof(current.ssid));
    strlcpy(current.password, CONFIG_CHAT_WIFI_PASSWORD,
            sizeof(current.password));
    strlcpy(current.backend_url, CONFIG_CHAT_BACKEND_URL,
            sizeof(current.backend_url));
    strlcpy(current.model, CONFIG_CHAT_BACKEND_MODEL, sizeof(current.model));
    strlcpy(current.mode, CONFIG_CHAT_DEFAULT_MODE, sizeof(current.mode));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    app_settings_t saved = {0};
    load_string(handle, "ssid", saved.ssid, sizeof(saved.ssid));
    load_string(handle, "password", saved.password, sizeof(saved.password));
    load_string(handle, "backend", saved.backend_url,
                sizeof(saved.backend_url));
    load_string(handle, "model", saved.model, sizeof(saved.model));
    load_string(handle, "mode", saved.mode, sizeof(saved.mode));
    nvs_close(handle);

    if (saved.ssid[0]) {
        strlcpy(current.ssid, saved.ssid, sizeof(current.ssid));
        strlcpy(current.password, saved.password, sizeof(current.password));
    }
    if (saved.backend_url[0]) {
        strlcpy(current.backend_url, saved.backend_url,
                sizeof(current.backend_url));
    }
    if (saved.model[0]) {
        strlcpy(current.model, saved.model, sizeof(current.model));
    }
    if (saved.mode[0]) {
        strlcpy(current.mode, saved.mode, sizeof(current.mode));
    }
    return ESP_OK;
}

void settings_store_get(app_settings_t *settings)
{
    if (!settings || !lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    *settings = current;
    xSemaphoreGive(lock);
}

esp_err_t settings_store_save(const app_settings_t *settings)
{
    if (!settings || !lock) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    if ((err = nvs_set_str(handle, "ssid", settings->ssid)) == ESP_OK &&
        (err = nvs_set_str(handle, "password", settings->password)) == ESP_OK &&
        (err = nvs_set_str(handle, "backend",
                           settings->backend_url)) == ESP_OK &&
        (err = nvs_set_str(handle, "model", settings->model)) == ESP_OK &&
        (err = nvs_set_str(handle, "mode", settings->mode)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    current = *settings;
    xSemaphoreGive(lock);
    return ESP_OK;
}
