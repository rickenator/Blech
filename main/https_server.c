#include "https_server.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "chat_service.h"
#include "status_led.h"
#include "honeypot_log.h"
#include "settings_store.h"
#include "wifi_manager.h"

static const char *TAG = "web";
static httpd_handle_t http_server;
static httpd_handle_t https_server;

extern const unsigned char index_html_start[]
    asm("_binary_index_html_start");
extern const unsigned char index_html_end[]
    asm("_binary_index_html_end");

static esp_err_t root_get(httpd_req_t *req)
{
    status_led_set(STATUS_LED_AP_ACTIVE);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start - 1);
}

static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "Open Blech");
}

static esp_err_t status_get(httpd_req_t *req)
{
    app_settings_t settings;
    char ip[16];
    settings_store_get(&settings);
    wifi_manager_station_ip(ip, sizeof(ip));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", chat_service_model_ready());
    cJSON_AddBoolToObject(root, "lan_ready", chat_service_lan_ready());
    cJSON_AddBoolToObject(root, "local_ready",
                          chat_service_local_ready());
    cJSON_AddBoolToObject(root, "wifi_connected",
                          wifi_manager_station_connected());
    cJSON_AddStringToObject(root, "mode", settings.mode);
    cJSON_AddStringToObject(root, "station_ip", ip);
    honeypot_log_http("GET", "/api/status", 200);
    status_led_set(STATUS_LED_TALKING);
    cJSON_AddStringToObject(root, "status", chat_service_model_status());
    cJSON_AddStringToObject(root, "local_status",
                            chat_service_local_status());
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static char *receive_body(httpd_req_t *req, size_t maximum)
{
    if (req->content_len <= 0 || req->content_len > maximum) {
        return NULL;
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        return NULL;
    }
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received,
                               req->content_len - received);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        received += n;
    }
    return body;
}

static esp_err_t chat_post(httpd_req_t *req)
{
    status_led_set(STATUS_LED_THINKING);
    char *request = receive_body(req, CONFIG_CHAT_MAX_REQUEST_BYTES);
    char *response = calloc(1, CONFIG_CHAT_MAX_RESPONSE_BYTES);
    if (!request || !response) {
        free(request);
        free(response);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid or oversized chat request");
        return ESP_FAIL;
    }

    esp_err_t err = chat_service_generate_request(
        request, response, CONFIG_CHAT_MAX_RESPONSE_BYTES);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (err != ESP_OK) {
        httpd_resp_set_status(
            req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request"
                                            : "503 Service Unavailable");
    }
    esp_err_t send_err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    free(request);
    free(response);
    return send_err;
}

static esp_err_t config_get(httpd_req_t *req)
{
    app_settings_t settings;
    settings_store_get(&settings);
    cJSON *root = cJSON_CreateObject();
    honeypot_log_http("GET", "/api/config", 200);
    status_led_set(STATUS_LED_TALKING);
    cJSON_AddStringToObject(root, "ssid", settings.ssid);
    cJSON_AddStringToObject(root, "backend_url", settings.backend_url);
    cJSON_AddStringToObject(root, "model", settings.model);
    cJSON_AddStringToObject(root, "mode", settings.mode);
    cJSON_AddBoolToObject(root, "has_password", settings.password[0] != '\0');
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static bool valid_string(cJSON *item, size_t maximum)
{
    return cJSON_IsString(item) && strlen(item->valuestring) <= maximum;
}

static esp_err_t config_post(httpd_req_t *req)
{
    char *body = receive_body(req, 1024);
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid settings JSON");
    }
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    cJSON *backend = cJSON_GetObjectItemCaseSensitive(root, "backend_url");
    cJSON *model = cJSON_GetObjectItemCaseSensitive(root, "model");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    bool valid = valid_string(ssid, SETTINGS_SSID_MAX) &&
                 valid_string(backend, SETTINGS_BACKEND_URL_MAX) &&
                 valid_string(model, SETTINGS_MODEL_MAX) &&
                 valid_string(mode, SETTINGS_MODE_MAX) &&
                 (!password ||
                  valid_string(password, SETTINGS_PASSWORD_MAX)) &&
                 (!strcmp(mode->valuestring, "auto") ||
                  !strcmp(mode->valuestring, "lan") ||
                  !strcmp(mode->valuestring, "local"));
    if (!valid) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid settings");
    }

    app_settings_t settings;
    settings_store_get(&settings);
    strlcpy(settings.ssid, ssid->valuestring, sizeof(settings.ssid));
    if (password) {
        strlcpy(settings.password, password->valuestring,
                sizeof(settings.password));
    }
    strlcpy(settings.backend_url, backend->valuestring,
            sizeof(settings.backend_url));
    strlcpy(settings.model, model->valuestring, sizeof(settings.model));
    strlcpy(settings.mode, mode->valuestring, sizeof(settings.mode));
    status_led_set(STATUS_LED_TALKING);
    honeypot_log_provision("config_save", settings.ssid[0] ? settings.ssid : "cleared");
    esp_err_t err = settings_store_save(&settings);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        err = wifi_manager_apply_station();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings apply failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "settings saved but Wi-Fi apply failed");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"saved\":true}");
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    honeypot_log_http("GET", "/api/wifi-scan", 200);
    status_led_set(STATUS_LED_TALKING);
    wifi_scan_ap_t aps[WIFI_SCAN_MAX_AP];
    int count = wifi_manager_scan(aps, WIFI_SCAN_MAX_AP);
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(item, "secure", aps[i].secure);
        cJSON_AddItemToArray(list, item);
    }
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    return err;
}

static bool stream_on_token(const char *text, void *ctx)
{
    httpd_req_t *req = (httpd_req_t *)ctx;
    httpd_resp_send_chunk(req, text, HTTPD_RESP_USE_STRLEN);
    return true;
}

static esp_err_t chat_post_stream(httpd_req_t *req)
{
    honeypot_log_http("POST", "/api/chat/stream", 200);
    status_led_set(STATUS_LED_THINKING);

    char *request = receive_body(req, CONFIG_CHAT_MAX_REQUEST_BYTES);
    char *response = calloc(1, CONFIG_CHAT_MAX_RESPONSE_BYTES);
    if (!request || !response) {
        free(request);
        free(response);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "invalid or oversized request");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = chat_service_generate_request_stream(
        request, response, CONFIG_CHAT_MAX_RESPONSE_BYTES,
        stream_on_token, req);
    httpd_resp_send_chunk(req, NULL, 0);
    free(request);
    free(response);
    return err;
}

static esp_err_t register_routes(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/wifi-scan", .method = HTTP_GET, .handler = wifi_scan_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/chat", .method = HTTP_POST, .handler = chat_post},
        {.uri = "/api/chat/stream", .method = HTTP_POST, .handler = chat_post_stream},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]),
                            TAG, "register route");
    }
    return ESP_OK;
}

esp_err_t https_server_start(void)
{
    if (http_server || https_server) {
        return ESP_OK;
    }
    extern const unsigned char servercert_start[]
        asm("_binary_servercert_pem_start");
    extern const unsigned char servercert_end[]
        asm("_binary_servercert_pem_end");
    extern const unsigned char prvtkey_start[]
        asm("_binary_prvtkey_pem_start");
    extern const unsigned char prvtkey_end[]
        asm("_binary_prvtkey_pem_end");

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.server_port = 80;
    http_config.core_id = 1;
    http_config.stack_size = 12288;
    http_config.max_uri_handlers = 8;
    ESP_RETURN_ON_ERROR(httpd_start(&http_server, &http_config),
                        TAG, "start HTTP server");
    ESP_RETURN_ON_ERROR(register_routes(http_server),
                        TAG, "register HTTP routes");
    ESP_RETURN_ON_ERROR(
        httpd_register_err_handler(http_server, HTTPD_404_NOT_FOUND,
                                   captive_redirect),
        TAG, "register captive redirect");
    ESP_LOGI(TAG, "HTTP listening on port 80");

    httpd_ssl_config_t https_config = HTTPD_SSL_CONFIG_DEFAULT();
    https_config.httpd.server_port = CONFIG_CHAT_HTTPS_PORT;
    https_config.httpd.core_id = 1;
    https_config.servercert = servercert_start;
    https_config.servercert_len = servercert_end - servercert_start;
    https_config.prvtkey_pem = prvtkey_start;
    https_config.prvtkey_len = prvtkey_end - prvtkey_start;
    https_config.httpd.stack_size = 12288;
    https_config.httpd.max_uri_handlers = 8;
    ESP_RETURN_ON_ERROR(httpd_ssl_start(&https_server, &https_config),
                        TAG, "start HTTPS server");
    ESP_RETURN_ON_ERROR(register_routes(https_server),
                        TAG, "register HTTPS routes");
    ESP_LOGI(TAG, "HTTPS listening on port %d", CONFIG_CHAT_HTTPS_PORT);
    return ESP_OK;
}

esp_err_t https_server_stop(void)
{
    esp_err_t result = ESP_OK;
    if (https_server) {
        esp_err_t err = httpd_ssl_stop(https_server);
        if (err == ESP_OK) {
            https_server = NULL;
        } else {
            result = err;
        }
    }
    if (http_server) {
        esp_err_t err = httpd_stop(http_server);
        if (err == ESP_OK) {
            http_server = NULL;
        } else if (result == ESP_OK) {
            result = err;
        }
    }
    return result;
}
