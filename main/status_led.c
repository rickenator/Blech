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

static int gpio1 = CONFIG_STATUS_LED_GPIO;   /* status — GPIO48 */
static int gpio2 = CONFIG_STATUS_LED2_GPIO;  /* activity — GPIO47 */
static esp_timer_handle_t led_timer;
static volatile status_led_state_t current_state = STATUS_LED_OFF;
static volatile int tick;
static volatile bool active;

static inline void set1(int v) { if (gpio1 >= 0) gpio_set_level(gpio1, v); }
static inline void set2(int v) { if (gpio2 >= 0) gpio_set_level(gpio2, v); }

static void timer_cb(void *arg)
{
    (void)arg;
    tick++;
    int t = tick;

    switch (current_state) {
    case STATUS_LED_OFF:
        set1(0); set2(0);
        break;

    case STATUS_LED_BOOTING:
        /* frantic 8Hz asymmetric: LED1 flicker, LED2 heartbeat */
        set1((t & 1) ? 0 : 1);
        set2((t % 8) < 2 ? 1 : 0);
        break;

    case STATUS_LED_AP_ACTIVE:
        /* slow breathe with LED2 occasional contempt-twitch */
        set1((t & 3) < 2 ? 1 : 0);
        set2((t % 20) == 0 ? 1 : 0);
        break;

    case STATUS_LED_CONNECTING:
        /* impatient: LED1 double-tap, LED2 off */
        set1((t % 6) < 2 || (t % 6) == 3 ? 1 : 0);
        set2(0);
        break;

    case STATUS_LED_CONNECTED:
        /* smug solid LED1, occasional LED2 wink */
        set1(1);
        set2((t % 60) < 2 ? 1 : 0);  /* wink every ~7.5s */
        break;

    case STATUS_LED_THINKING:
        /* chaser pattern: LED1→LED2→both off */
        set1((t % 4) == 0 ? 1 : 0);
        set2((t % 4) == 1 ? 1 : 0);
        break;

    case STATUS_LED_TALKING:
        /* LED2 stutter-sync with token output, LED1 aggressive flash */
        set1((t & 1) ? 0 : 1);
        set2((t % 3) == 0 ? 1 : 0);
        break;

    case STATUS_LED_ERROR:
        /* SOS triple: ... --- ... at LED speed */
        {
            int phase = t % 24;
            if (phase < 2 || (phase >= 4 && phase < 6) || (phase >= 8 && phase < 10))
                { set1(1); set2(1); }
            else if (phase >= 12 && phase < 20)
                { set1(1); set2(1); }
            else
                { set1(0); set2(0); }
        }
        break;
    }
}

esp_err_t status_led_init(void)
{
    if (gpio1 < 0 && gpio2 < 0) {
        ESP_LOGW(TAG, "no LED GPIO configured, skipping");
        return ESP_OK;
    }

    if (gpio1 >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio1),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio1, 0);
        ESP_LOGI(TAG, "status LED on GPIO%d", gpio1);
    }

    if (gpio2 >= 0) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio2),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio2, 0);
        ESP_LOGI(TAG, "activity LED on GPIO%d", gpio2);
    }

    esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name = "led_timer",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &led_timer),
                        TAG, "create LED timer");
    current_state = STATUS_LED_BOOTING;
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(led_timer, 125000),   /* 125ms → 8Hz base */
        TAG, "start LED timer");

    ESP_LOGI(TAG, "LED timer started");
    return ESP_OK;
}

void status_led_set(status_led_state_t state)
{
    if (gpio1 < 0 && gpio2 < 0) return;
    if (state == current_state) return;
    current_state = state;
    tick = 0;
}
