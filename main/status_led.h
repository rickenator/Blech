#pragma once
#include "esp_err.h"

typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_BOOTING,       /* frantic flicker — angry wake-up */
    STATUS_LED_AP_ACTIVE,      /* slow breath with attitude flick */
    STATUS_LED_CONNECTING,     /* impatient double-tap */
    STATUS_LED_CONNECTED,      /* smug solid */
    STATUS_LED_THINKING,       /* chaser pattern — "computing contempt" */
    STATUS_LED_TALKING,        /* LED2 stutter, LED1 flick — streaming insult */
    STATUS_LED_ERROR,          /* SOS-like triple */
} status_led_state_t;

esp_err_t status_led_init(void);
void status_led_set(status_led_state_t state);
