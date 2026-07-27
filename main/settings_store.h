#pragma once

#include "esp_err.h"

#define SETTINGS_SSID_MAX 32
#define SETTINGS_PASSWORD_MAX 64
#define SETTINGS_BACKEND_URL_MAX 159
#define SETTINGS_MODEL_MAX 127
#define SETTINGS_MODE_MAX 7

typedef struct {
    char ssid[SETTINGS_SSID_MAX + 1];
    char password[SETTINGS_PASSWORD_MAX + 1];
    char backend_url[SETTINGS_BACKEND_URL_MAX + 1];
    char model[SETTINGS_MODEL_MAX + 1];
    char mode[SETTINGS_MODE_MAX + 1];
} app_settings_t;

esp_err_t settings_store_init(void);
void settings_store_get(app_settings_t *settings);
esp_err_t settings_store_save(const app_settings_t *settings);
