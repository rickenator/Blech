#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "led";

#ifndef CONFIG_STATUS_LED_GPIO
#define CONFIG_STATUS_LED_GPIO -1
#endif
#ifndef CONFIG_STATUS_LED2_GPIO
#define CONFIG_STATUS_LED2_GPIO -1
#endif

static int gpio_pin = CONFIG_STATUS_LED_GPIO;
static int gpio_pin2 = CONFIG_STATUS_LED2_GPIO;
static esp_timer_handle_t led_timer;
static volatile status_led_state_t current_state = STATUS_LED_OFF;
static volatile int tick;

static void led_on(int pin)
{
    if (pin < 0) return;
    gpio_set_level(pin, 1);
}

static void led_off(int pin)
{
    if (pin < 0) return;
    gpio_set_level(pin, 0);
}

static void led_update(int pin, bool on)
{
    if (pin < 0) return;
    gpio_set_level(pin, on ? 1 : 0);
}

static void timer_cb(void *arg)
{
    (void)arg;
    tick++;

    switch (current_state) {
    case STATUS_LED_OFF:
        led_off(gpio_pin);
        led_off(gpio_pin2);
        break;

    case STATUS_LED_BOOTING:
        // fast blink ~4Hz: on for 1 tick, off for 1 tick
        led_update(gpio_pin, (tick & 1) == 0);
        led_off(gpio_pin2);
        break;

    case STATUS_LED_AP_ACTIVE:
        // slow steady blink ~1Hz: on for 2 ticks, off for 2 ticks
        led_update(gpio_pin, (tick & 3) < 2);
        led_off(gpio_pin2);
        break;

    case STATUS_LED_CONNECTING:
        // rapid double-blink: on-off-on-off-off-off
        led_update(gpio_pin, (tick % 6) < 2 || (tick % 6) == 3);
        led_off(gpio_pin2);
        break;

    case STATUS_LED_CONNECTED:
        // solid on
        led_on(gpio_pin);
        led_off(gpio_pin2);
        break;

    case STATUS_LED_INFERENCE:
        // breathing slow pulse: LED2 rapid flicker, LED1 solid
        led_on(gpio_pin);
        led_update(gpio_pin2, (tick & 1) == 0);
        break;

    case STATUS_LED_ERROR:
        // triple flash: on-off-on-off-on-off-off-off
        led_update(gpio_pin, (tick % 8) < 5 && (tick & 1) == 0);
        led_off(gpio_pin2);
        break;
    }
}

esp_err_t status_led_init(void)
{
    if (gpio_pin < 0 && gpio_pin2 < 0) {
        ESP_LOGW(TAG, "no LED GPIO configured, skipping");
        return ESP_OK;
    }

    if (gpio_pin >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio_pin, 0);
        ESP_LOGI(TAG, "status LED on GPIO %d", gpio_pin);
    }

    if (gpio_pin2 >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio_pin2),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio_pin2, 0);
        ESP_LOGI(TAG, "activity LED on GPIO %d", gpio_pin2);
    }

    esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name = "led_timer",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &led_timer),
                        TAG, "create LED timer");
    current_state = STATUS_LED_BOOTING;
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(led_timer, 125000),   // 125ms → 8Hz base
        TAG, "start LED timer");

    ESP_LOGI(TAG, "LED timer started");
    return ESP_OK;
}

void status_led_set(status_led_state_t state)
{
    if (gpio_pin < 0 && gpio_pin2 < 0) return;
    if (state == current_state) return;
    current_state = state;
    tick = 0;
}
