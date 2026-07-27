#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef bool (*llm_token_cb_t)(const char *text, void *ctx);

esp_err_t llm_init(void);
esp_err_t llm_generate(const char *prompt, char *response, size_t max_len);
esp_err_t llm_generate_stream(const char *prompt,
                              llm_token_cb_t on_token, void *ctx,
                              char *full_response, size_t max_len);
const char *llm_get_info(void);
