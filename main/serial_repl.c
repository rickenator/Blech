#include "serial_repl.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "chat_service.h"
#include "sdkconfig.h"

static const char *TAG = "repl";

#define CHAT_MAX_RESPONSE CONFIG_CHAT_MAX_RESPONSE_BYTES
#define REPL_BUF 1024

static bool serial_on_token(const char *text, void *ctx)
{
    (void)ctx;
    printf("%s", text);
    fflush(stdout);
    return true;
}

static void serial_repl_task(void *arg)
{
    (void)arg;
    printf("\n=== Blech serial REPL ===\n");
    printf("Type a message and press Enter.\n\n");

    char line[REPL_BUF];
    int idx = 0;
    char chat_response[CHAT_MAX_RESPONSE];

    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (idx == 0 && ch != '\r' && ch != '\n') {
            printf("> ");
            fflush(stdout);
        }

        if (ch == '\r' || ch == '\n') {
            if (idx == 0) continue;
            line[idx] = '\0';
            printf("\n");

            char request[2048];
            int written = snprintf(request, sizeof(request),
                "{\"messages\":["
                "{\"role\":\"user\",\"content\":\"%.1024s\"}]}",
                line);
            if (written < 0 || written >= (int)sizeof(request)) {
                printf("[request too long]\n\n");
                idx = 0;
                continue;
            }

            memset(chat_response, 0, sizeof(chat_response));
            esp_err_t err = chat_service_generate_request_stream(
                request, chat_response, CHAT_MAX_RESPONSE,
                serial_on_token, NULL);
            printf("\n\n");
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
                printf("[err %d]\n\n", err);
            }
            idx = 0;
        } else if (ch == 0x08 || ch == 0x7f) {
            if (idx > 0) { idx--; printf("\b \b"); fflush(stdout); }
        } else if (ch >= 0x20 && ch < 0x7f && idx < REPL_BUF - 1) {
            line[idx++] = (char)ch;
            printf("%c", ch);
            fflush(stdout);
        }
    }
}

esp_err_t serial_repl_start(void)
{
    if (xTaskCreate(serial_repl_task, "repl", 8192, NULL, 3, NULL)
        != pdPASS) {
        ESP_LOGE(TAG, "REPL task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "serial REPL ready on console UART");
    return ESP_OK;
}
