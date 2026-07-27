#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef bool (*chat_token_cb_t)(const char *token, void *ctx);

esp_err_t chat_service_start(void);
esp_err_t chat_service_generate(const char *prompt, char *response,
                                size_t response_size);
esp_err_t chat_service_generate_request(const char *request_json,
                                        char *response,
                                        size_t response_size);
esp_err_t chat_service_generate_request_stream(
    const char *request_json, char *response, size_t response_size,
    chat_token_cb_t on_token, void *ctx);
bool chat_service_model_ready(void);
const char *chat_service_model_status(void);
bool chat_service_local_ready(void);
bool chat_service_lan_ready(void);
const char *chat_service_local_status(void);
