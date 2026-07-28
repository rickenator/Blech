#include "chat_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "llm_inference.h"
#include "sdkconfig.h"
#include "settings_store.h"
#include "honeypot_log.h"
#include "wifi_manager.h"
#include "status_led.h"

static const char *TAG = "agent";
static SemaphoreHandle_t agent_lock;
static char service_status[192] = "waiting for Wi-Fi configuration";
static bool local_ready;
static char local_status[160] = "local model not initialized";

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} http_buffer_t;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    http_buffer_t *buffer = event->user_data;
    if (!buffer || buffer->length + event->data_len + 1 > buffer->capacity) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buffer->data + buffer->length, event->data, event->data_len);
    buffer->length += event->data_len;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static cJSON *tool_definition(const char *name, const char *description)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON *function = cJSON_AddObjectToObject(tool, "function");
    cJSON_AddStringToObject(tool, "type", "function");
    cJSON_AddStringToObject(function, "name", name);
    cJSON_AddStringToObject(function, "description", description);
    cJSON *parameters = cJSON_AddObjectToObject(function, "parameters");
    cJSON_AddStringToObject(parameters, "type", "object");
    cJSON_AddObjectToObject(parameters, "properties");
    cJSON_AddArrayToObject(parameters, "required");
    return tool;
}

static cJSON *build_tools(void)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON_AddItemToArray(
        tools,
        tool_definition("get_device_status",
                        "Get live ESP32 uptime, memory, Wi-Fi IP, and signal"));
    cJSON_AddItemToArray(
        tools,
        tool_definition("get_agent_capabilities",
                        "List tools this ESP32 agent is allowed to execute"));
    return tools;
}

static char *execute_tool(const char *name)
{
    cJSON *result = cJSON_CreateObject();
    if (!strcmp(name, "get_device_status")) {
        char ip[16];
        char ssid[SETTINGS_SSID_MAX + 1];
        wifi_manager_station_ip(ip, sizeof(ip));
        wifi_manager_station_ssid(ssid, sizeof(ssid));
        cJSON_AddStringToObject(result, "device", "ESP32-S3");
        cJSON_AddNumberToObject(result, "uptime_seconds",
                                esp_timer_get_time() / 1000000ULL);
        cJSON_AddNumberToObject(
            result, "free_internal_bytes",
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(
            result, "free_psram_bytes",
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddBoolToObject(result, "wifi_connected",
                              wifi_manager_station_connected());
        cJSON_AddStringToObject(result, "wifi_ssid", ssid);
        cJSON_AddStringToObject(result, "station_ip", ip);
        cJSON_AddNumberToObject(result, "wifi_rssi",
                                wifi_manager_station_rssi());
    } else if (!strcmp(name, "get_agent_capabilities")) {
        cJSON *tools = cJSON_AddArrayToObject(result, "tools");
        cJSON_AddItemToArray(tools,
                            cJSON_CreateString("get_device_status"));
        cJSON_AddItemToArray(tools,
                            cJSON_CreateString("get_agent_capabilities"));
        cJSON_AddStringToObject(
            result, "policy",
            "Read-only device tools; maximum bounded tool-loop steps");
    } else {
        cJSON_AddStringToObject(result, "error", "tool is not permitted");
    }
    char *text = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return text;
}

static bool format_local_tool_result(const char *name, const char *text,
                                     char *response, size_t response_size)
{
    cJSON *result = cJSON_Parse(text);
    if (!result) {
        return false;
    }
    if (!strcmp(name, "get_device_status")) {
        cJSON *psram = cJSON_GetObjectItemCaseSensitive(
            result, "free_psram_bytes");
        cJSON *internal = cJSON_GetObjectItemCaseSensitive(
            result, "free_internal_bytes");
        cJSON *uptime = cJSON_GetObjectItemCaseSensitive(
            result, "uptime_seconds");
        cJSON *connected = cJSON_GetObjectItemCaseSensitive(
            result, "wifi_connected");
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(result, "wifi_ssid");
        cJSON *ip = cJSON_GetObjectItemCaseSensitive(result, "station_ip");
        if (cJSON_IsNumber(psram) && cJSON_IsNumber(internal) &&
            cJSON_IsNumber(uptime) && cJSON_IsBool(connected)) {
            if (cJSON_IsTrue(connected) && cJSON_IsString(ssid) &&
                cJSON_IsString(ip)) {
                snprintf(
                    response, response_size,
                    "%.2f MiB PSRAM and %.1f KiB internal RAM are free. "
                    "Uptime is %.0f seconds; Wi-Fi is connected to %s at %s.",
                    psram->valuedouble / (1024.0 * 1024.0),
                    internal->valuedouble / 1024.0, uptime->valuedouble,
                    ssid->valuestring, ip->valuestring);
            } else {
                snprintf(
                    response, response_size,
                    "%.2f MiB PSRAM and %.1f KiB internal RAM are free. "
                    "Uptime is %.0f seconds; station Wi-Fi is disconnected.",
                    psram->valuedouble / (1024.0 * 1024.0),
                    internal->valuedouble / 1024.0, uptime->valuedouble);
            }
            cJSON_Delete(result);
            return true;
        }
    } else if (!strcmp(name, "get_agent_capabilities")) {
        snprintf(response, response_size,
                 "I can read this ESP32's live status and list my "
                 "capabilities. Both tools are read-only.");
        cJSON_Delete(result);
        return true;
    }
    cJSON_Delete(result);
    return false;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (!needle_len) {
        return true;
    }
    for (; *text; text++) {
        size_t i = 0;
        while (i < needle_len && text[i] &&
               tolower((unsigned char)text[i]) ==
                   tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static esp_err_t try_fast_local_tool(cJSON *messages, char *response,
                                     size_t response_size)
{
    for (int i = cJSON_GetArraySize(messages) - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
        if (!cJSON_IsString(role) || strcmp(role->valuestring, "user") ||
            !cJSON_IsString(content)) {
            continue;
        }
        const char *tool_name = NULL;
        const char *text = content->valuestring;
        if (contains_case_insensitive(text, "psram") ||
            contains_case_insensitive(text, "free memory") ||
            contains_case_insensitive(text, "device status") ||
            contains_case_insensitive(text, "uptime") ||
            contains_case_insensitive(text, "wifi signal")) {
            tool_name = "get_device_status";
        } else if (contains_case_insensitive(text, "capabilities") ||
                   contains_case_insensitive(text, "what tools")) {
            tool_name = "get_agent_capabilities";
        }
        if (!tool_name) {
            return ESP_ERR_NOT_FOUND;
        }
        char *tool_result = execute_tool(tool_name);
        if (!tool_result) {
            return ESP_ERR_NO_MEM;
        }
        bool formatted = format_local_tool_result(
            tool_name, tool_result, response, response_size);
        free(tool_result);
        return formatted ? ESP_OK : ESP_FAIL;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t backend_message(cJSON *messages, cJSON **message_out)
{
    app_settings_t settings;
    settings_store_get(&settings);
    if (!wifi_manager_station_connected()) {
        snprintf(service_status, sizeof(service_status),
                 "Connect the ESP32 to home Wi-Fi in Settings");
        return ESP_ERR_INVALID_STATE;
    }
    if (!settings.backend_url[0] || !settings.model[0]) {
        snprintf(service_status, sizeof(service_status),
                 "Configure the model backend in Settings");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "model", settings.model);
    cJSON_AddItemReferenceToObject(request, "messages", messages);
    cJSON_AddItemToObject(request, "tools", build_tools());
    cJSON_AddStringToObject(request, "tool_choice", "auto");
    cJSON_AddNumberToObject(request, "temperature", 0.3);
    cJSON_AddNumberToObject(request, "max_tokens", 512);
    cJSON_AddBoolToObject(request, "stream", false);
    char *request_text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!request_text) {
        return ESP_ERR_NO_MEM;
    }

    http_buffer_t response = {
        .capacity = 65536,
        .data = heap_caps_calloc(1, 65536, MALLOC_CAP_SPIRAM |
                                           MALLOC_CAP_8BIT),
    };
    if (!response.data) {
        response.data = calloc(1, response.capacity);
    }
    if (!response.data) {
        free(request_text);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = settings.backend_url,
        .event_handler = http_event,
        .user_data = &response,
        .timeout_ms = 60000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(request_text);
        free(response.data);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_text, strlen(request_text));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(request_text);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGE(TAG, "backend request failed: %s, HTTP %d",
                 esp_err_to_name(err), status);
        snprintf(service_status, sizeof(service_status),
                 "Backend unavailable: %s, HTTP %d",
                 esp_err_to_name(err), status);
        free(response.data);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    cJSON *root = cJSON_Parse(response.data);
    free(response.data);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *choice = cJSON_IsArray(choices)
                        ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice
                         ? cJSON_GetObjectItemCaseSensitive(choice, "message")
                         : NULL;
    if (!cJSON_IsObject(message)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *message_out = cJSON_Duplicate(message, true);
    cJSON_Delete(root);
    if (!*message_out) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(service_status, sizeof(service_status),
             "Agent ready via %s", settings.model);
    return ESP_OK;
}

static esp_err_t run_local_stream(cJSON *messages, char *response, size_t response_size, chat_token_cb_t on_token, void *ctx);
static esp_err_t run_agent(cJSON *messages, char *response,
                           size_t response_size)
{
    status_led_set(STATUS_LED_THINKING);
    for (int step = 0; step < CONFIG_CHAT_MAX_AGENT_STEPS; step++) {
        cJSON *message = NULL;
        esp_err_t err = backend_message(messages, &message);
        if (err != ESP_OK) {
            snprintf(response, response_size, "%s", service_status);
            return err;
        }

        cJSON *tool_calls =
            cJSON_GetObjectItemCaseSensitive(message, "tool_calls");
        if (!cJSON_IsArray(tool_calls) ||
            cJSON_GetArraySize(tool_calls) == 0) {
            cJSON *content =
                cJSON_GetObjectItemCaseSensitive(message, "content");
            snprintf(response, response_size, "%s",
                     cJSON_IsString(content) ? content->valuestring
                                             : "The backend returned no text.");
            cJSON_Delete(message);
            return ESP_OK;
        }

        cJSON_AddItemToArray(messages, message);
        cJSON *call;
        cJSON_ArrayForEach(call, tool_calls) {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(call, "id");
            cJSON *function =
                cJSON_GetObjectItemCaseSensitive(call, "function");
            cJSON *name = function
                              ? cJSON_GetObjectItemCaseSensitive(function,
                                                                 "name")
                              : NULL;
            const char *tool_name =
                cJSON_IsString(name) ? name->valuestring : "";
            char *tool_result = execute_tool(tool_name);
            cJSON *tool_message = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_message, "role", "tool");
            cJSON_AddStringToObject(
                tool_message, "tool_call_id",
                cJSON_IsString(id) ? id->valuestring : "unknown");
            cJSON_AddStringToObject(tool_message, "content",
                                    tool_result ? tool_result
                                                : "{\"error\":\"no memory\"}");
            free(tool_result);
            cJSON_AddItemToArray(messages, tool_message);
        }
    }
    snprintf(response, response_size,
             "The agent stopped after %d tool steps.",
             CONFIG_CHAT_MAX_AGENT_STEPS);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t run_local(cJSON *messages, char *response,
                           size_t response_size)
{
    status_led_set(STATUS_LED_THINKING);
    if (!local_ready) {
        snprintf(response, response_size,
                 "Local dialogue model is not installed yet. "
                 "Use LAN mode or install the new model assets.");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t fast = try_fast_local_tool(
        messages, response, response_size);
    if (fast != ESP_ERR_NOT_FOUND) {
        return fast;
    }
    char *transcript = heap_caps_calloc(
        1, CONFIG_CHAT_MAX_PROMPT_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!transcript) {
        transcript = calloc(1, CONFIG_CHAT_MAX_PROMPT_BYTES + 1);
    }
    if (!transcript) {
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    int count = cJSON_GetArraySize(messages);
    int first = count > 8 ? count - 8 : 0;
    for (int i = first; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
        if (!cJSON_IsString(role) || !cJSON_IsString(content)) {
            continue;
        }
        // The local checkpoint was trained on user/assistant turns without a
        // system preamble. Keep the richer system message for LAN backends,
        // but do not spend ESP32 prefill time on out-of-distribution text.
        if (!strcmp(role->valuestring, "system")) {
            continue;
        }
        int written = snprintf(
            transcript + used, CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used,
            "<|%s|>\n%s\n", role->valuestring, content->valuestring);
        if (written < 0 ||
            (size_t)written >= CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used) {
            free(transcript);
            snprintf(response, response_size,
                     "Conversation is too long for the local model.");
            return ESP_ERR_INVALID_SIZE;
        }
        used += written;
    }
    int written = snprintf(
        transcript + used, CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used,
        "<|assistant|>\n");
    if (written < 0 ||
        (size_t)written >= CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used) {
        free(transcript);
        return ESP_ERR_INVALID_SIZE;
    }
    used += written;

    esp_err_t err = llm_generate(transcript, response, response_size);
    if (err != ESP_OK) {
        free(transcript);
        return err;
    }

    const char *tag = strstr(response, "<tool_call>");
    const char *end = tag ? strstr(tag, "</tool_call>") : NULL;
    if (!tag || !end) {
        free(transcript);
        return ESP_OK;
    }
    tag += strlen("<tool_call>");
    cJSON *call = cJSON_ParseWithLength(tag, end - tag);
    cJSON *name = call
                      ? cJSON_GetObjectItemCaseSensitive(call, "name")
                      : NULL;
    const char *tool_name =
        cJSON_IsString(name) ? name->valuestring : "";
    if (strcmp(tool_name, "get_device_status") &&
        strcmp(tool_name, "get_agent_capabilities")) {
        cJSON_Delete(call);
        free(transcript);
        snprintf(response, response_size,
                 "I cannot run that tool on this device.");
        return ESP_ERR_NOT_SUPPORTED;
    }
    char *tool_result = execute_tool(tool_name);
    cJSON_Delete(call);
    if (!tool_result) {
        free(transcript);
        return ESP_ERR_NO_MEM;
    }
    if (format_local_tool_result(tool_name, tool_result,
                                 response, response_size)) {
        free(tool_result);
        free(transcript);
        return ESP_OK;
    }

    written = snprintf(
        transcript + used, CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used,
        "%s\n<|tool|>\n%s\n<|assistant|>\n", response, tool_result);
    free(tool_result);
    if (written < 0 ||
        (size_t)written >= CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used) {
        free(transcript);
        snprintf(response, response_size,
                 "The local tool result exceeded the context window.");
        return ESP_ERR_INVALID_SIZE;
    }
    err = llm_generate(transcript, response, response_size);
    free(transcript);
    return err;
}

static cJSON *validated_messages(const char *request_json)
{
    cJSON *request = cJSON_Parse(request_json);
    cJSON *input = request
                       ? cJSON_GetObjectItemCaseSensitive(request, "messages")
                       : NULL;
    if (!cJSON_IsArray(input)) {
        cJSON_Delete(request);
        return NULL;
    }

    cJSON *messages = cJSON_CreateArray();
    cJSON *system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(
        system, "content",
        "You are Buddy, a concise assistant reached through an ESP32-S3. "
        "Use the provided tools whenever the user asks about the device. "
        "Never invent tool results or claim actions outside the tool list.");
    cJSON_AddItemToArray(messages, system);

    int count = cJSON_GetArraySize(input);
    int first = count > 12 ? count - 12 : 0;
    for (int i = first; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(input, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
        if (!cJSON_IsString(role) || !cJSON_IsString(content) ||
            (strcmp(role->valuestring, "user") &&
             strcmp(role->valuestring, "assistant"))) {
            continue;
        }
        cJSON *copy = cJSON_CreateObject();
        cJSON_AddStringToObject(copy, "role", role->valuestring);
        cJSON_AddStringToObject(copy, "content", content->valuestring);
        cJSON_AddItemToArray(messages, copy);
    }
    cJSON_Delete(request);
    return messages;
}

esp_err_t chat_service_start(void)
{
    agent_lock = xSemaphoreCreateMutex();
    if (!agent_lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = llm_init();
    local_ready = err == ESP_OK;
    snprintf(local_status, sizeof(local_status), "%s",
             local_ready ? llm_get_info()
                         : "Local dialogue model assets not installed");
    ESP_LOGI(TAG, "%s", local_status);
    return ESP_OK;
}

esp_err_t chat_service_generate_request(const char *request_json,
                                        char *response,
                                        size_t response_size)
{
    if (!request_json || !response || !response_size) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *messages = validated_messages(request_json);
    if (!messages || cJSON_GetArraySize(messages) < 2) {
        cJSON_Delete(messages);
        snprintf(response, response_size, "No valid conversation messages.");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(agent_lock, pdMS_TO_TICKS(65000)) != pdTRUE) {
        cJSON_Delete(messages);
        snprintf(response, response_size, "The agent is busy.");
        return ESP_ERR_TIMEOUT;
    }
    app_settings_t settings;
    settings_store_get(&settings);
    esp_err_t err;
    if (!strcmp(settings.mode, "local")) {
        err = run_local(messages, response, response_size);
    } else {
        err = run_agent(messages, response, response_size);
        if (err != ESP_OK && !strcmp(settings.mode, "auto") &&
            local_ready) {
            ESP_LOGW(TAG, "LAN agent unavailable; falling back locally");
            err = run_local(messages, response, response_size);
        }
    }
    status_led_set(wifi_manager_station_connected() ? STATUS_LED_CONNECTED : STATUS_LED_AP_ACTIVE);
    xSemaphoreGive(agent_lock);
    cJSON_Delete(messages);
    return err;
}

esp_err_t chat_service_generate(const char *prompt, char *response,
                                size_t response_size)
{
    cJSON *request = cJSON_CreateObject();
    cJSON *messages = cJSON_AddArrayToObject(request, "messages");
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", prompt);
    cJSON_AddItemToArray(messages, message);
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err =
        chat_service_generate_request(text, response, response_size);
    free(text);
    return err;
}

bool chat_service_model_ready(void)
{
    app_settings_t settings;
    settings_store_get(&settings);
    bool lan_ready = wifi_manager_station_connected() &&
                     settings.backend_url[0] && settings.model[0];
    if (!strcmp(settings.mode, "local")) {
        return local_ready;
    }
    return lan_ready || (!strcmp(settings.mode, "auto") && local_ready);
}

const char *chat_service_model_status(void)
{
    app_settings_t settings;
    settings_store_get(&settings);
    if (!strcmp(settings.mode, "local")) {
        return local_status;
    }
    if (!wifi_manager_station_connected()) {
        return local_ready && !strcmp(settings.mode, "auto")
                   ? "LAN offline; local model ready"
                   : "Connect the ESP32 to home Wi-Fi in Settings";
    }
    if (!settings.backend_url[0] || !settings.model[0]) {
        return "Configure the model backend in Settings";
    }
    return service_status;
}

bool chat_service_local_ready(void)
{
    return local_ready;
}

bool chat_service_lan_ready(void)
{
    app_settings_t settings;
    settings_store_get(&settings);
    return wifi_manager_station_connected() &&
           settings.backend_url[0] && settings.model[0];
}

const char *chat_service_local_status(void)
{
    return local_status;
}


esp_err_t chat_service_generate_request_stream(
    const char *request_json, char *response, size_t response_size,
    chat_token_cb_t on_token, void *ctx)
{
    if (!request_json || !response || !response_size) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *messages = validated_messages(request_json);
    if (!messages || cJSON_GetArraySize(messages) < 2) {
        cJSON_Delete(messages);
        const char *err = "No valid conversation messages.";
        if (on_token) on_token(err, ctx);
        snprintf(response, response_size, "%s", err);
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(agent_lock, pdMS_TO_TICKS(65000)) != pdTRUE) {
        cJSON_Delete(messages);
        const char *err = "The agent is busy.";
        if (on_token) on_token(err, ctx);
        snprintf(response, response_size, "%s", err);
        return ESP_ERR_TIMEOUT;
    }
    app_settings_t settings;
    settings_store_get(&settings);
    bool is_local = !strcmp(settings.mode, "local");

    if (is_local) {
        esp_err_t err = run_local_stream(
            messages, response, response_size, on_token, ctx);
        status_led_set(wifi_manager_station_connected()
                           ? STATUS_LED_CONNECTED
                           : STATUS_LED_AP_ACTIVE);
        xSemaphoreGive(agent_lock);
        cJSON_Delete(messages);
        return err;
    }

    /* LAN mode: generate then push as one chunk */
    esp_err_t err = run_agent(messages, response, response_size);
    if (err != ESP_OK && !strcmp(settings.mode, "auto") && local_ready) {
        ESP_LOGW(TAG, "LAN agent unavailable; falling back locally");
        cJSON *m2 = cJSON_Duplicate(messages, 1);
        if (m2) {
            err = run_local_stream(
                m2, response, response_size, on_token, ctx);
            cJSON_Delete(m2);
        }
    } else if (err == ESP_OK && on_token) {
        on_token(response, ctx);
    }
    status_led_set(wifi_manager_station_connected()
                       ? STATUS_LED_CONNECTED
                       : STATUS_LED_AP_ACTIVE);
    xSemaphoreGive(agent_lock);
    cJSON_Delete(messages);
    return err;
}

static bool llm_callback_adapter(const char *text, void *ctx)
{
    chat_token_cb_t on_token = ((void **)ctx)[0];
    void *user_ctx = ((void **)ctx)[1];
    return on_token(text, user_ctx);
}

static esp_err_t run_local_stream(
    cJSON *messages, char *response, size_t response_size,
    chat_token_cb_t on_token, void *ctx)
{
    status_led_set(STATUS_LED_THINKING);
    if (!local_ready) {
        const char *err_str =
            "Local dialogue model is not installed yet.";
        if (on_token) on_token(err_str, ctx);
        snprintf(response, response_size, "%s", err_str);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t fast = try_fast_local_tool(
        messages, response, response_size);
    if (fast != ESP_ERR_NOT_FOUND) {
        if (on_token) on_token(response, ctx);
        return fast;
    }
    char *transcript = heap_caps_calloc(
        1, CONFIG_CHAT_MAX_PROMPT_BYTES + 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!transcript) {
        transcript = calloc(1, CONFIG_CHAT_MAX_PROMPT_BYTES + 1);
    }
    if (!transcript) return ESP_ERR_NO_MEM;

    size_t used = 0;
    int count = cJSON_GetArraySize(messages);
    int first = count > 8 ? count - 8 : 0;
    for (int i = first; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
        cJSON *content =
            cJSON_GetObjectItemCaseSensitive(item, "content");
        if (!cJSON_IsString(role) || !cJSON_IsString(content)) continue;
        if (!strcmp(role->valuestring, "system")) continue;
        int written = snprintf(
            transcript + used,
            CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used,
            "<|%s|>\n%s\n", role->valuestring,
            content->valuestring);
        if (written < 0 ||
            (size_t)written >=
                CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used) {
            free(transcript);
            return ESP_ERR_INVALID_SIZE;
        }
        used += written;
    }
    int written = snprintf(
        transcript + used,
        CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used,
        "<|assistant|>\n");
    if (written < 0 ||
        (size_t)written >=
            CONFIG_CHAT_MAX_PROMPT_BYTES + 1 - used) {
        free(transcript);
        return ESP_ERR_INVALID_SIZE;
    }
    used += written;

    void *adapter[2] = {(void *)on_token, ctx};
    esp_err_t err = llm_generate_stream(
        transcript,
        on_token ? llm_callback_adapter : NULL,
        on_token ? adapter : NULL,
        response, response_size);
    free(transcript);
    return err;
}
