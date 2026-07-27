#pragma once
#include "esp_err.h"

typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_BOOTING,
    STATUS_LED_AP_ACTIVE,
    STATUS_LED_CONNECTING,
    STATUS_LED_CONNECTED,
    STATUS_LED_INFERENCE,
    STATUS_LED_ERROR,
} status_led_state_t;

esp_err_t status_led_init(void);
void status_led_set(status_led_state_t state);
