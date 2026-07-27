#include "wifi_manager.h"

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "dns_server.h"
#include "settings_store.h"
#include "status_led.h"

static const char *TAG = "wifi";
static int reconnect_attempts;
static bool station_connected;
static bool station_configured;
static char station_ip[16];
static char station_ssid[SETTINGS_SSID_MAX + 1];
static dns_server_handle_t dns_server;
static const char captive_portal_uri[] = "http://192.168.4.1";

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (station_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        station_connected = false;
        station_ip[0] = '\0';
        if (station_configured && reconnect_attempts++ < 10) {
            esp_wifi_connect();
        } else if (station_configured) {
            ESP_LOGW(TAG, "station reconnect limit reached; AP remains active");
        }
        status_led_set(station_configured ? STATUS_LED_CONNECTING : STATUS_LED_AP_ACTIVE);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        reconnect_attempts = 0;
        station_connected = true;
        snprintf(station_ip, sizeof(station_ip), IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "station address: " IPSTR, IP2STR(&event->ip_info.ip));
        status_led_set(STATUS_LED_CONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "AP client " MACSTR " joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "AP client " MACSTR " left, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

esp_err_t wifi_manager_apply_station(void)
{
    app_settings_t settings;
    settings_store_get(&settings);
    station_configured = settings.ssid[0] != '\0';
    strlcpy(station_ssid, settings.ssid, sizeof(station_ssid));
    reconnect_attempts = 0;

    if (!station_configured) {
        station_connected = false;
        station_ip[0] = '\0';
        esp_err_t err = esp_wifi_disconnect();
        return err == ESP_ERR_WIFI_NOT_CONNECT ? ESP_OK : err;
    }

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, settings.ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, settings.password,
            sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta),
                        TAG, "set station config");
    esp_wifi_disconnect();
    status_led_set(STATUS_LED_CONNECTING);
    return esp_wifi_connect();
}

esp_err_t wifi_manager_start(void)
{
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(ap_netif, ESP_FAIL, TAG, "create AP network");

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   wifi_event, NULL),
        TAG, "register Wi-Fi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   wifi_event, NULL),
        TAG, "register IP handler");

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_CHAT_AP_SSID,
            sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, CONFIG_CHAT_AP_PASSWORD,
            sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(CONFIG_CHAT_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = strlen(CONFIG_CHAT_AP_PASSWORD) >= 8
                         ? WIFI_AUTH_WPA2_PSK
                         : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA),
                        TAG, "set Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap),
                        TAG, "set AP config");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }
    ESP_RETURN_ON_ERROR(
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_CAPTIVEPORTAL_URI,
                               (void *)captive_portal_uri,
                               strlen(captive_portal_uri)),
        TAG, "set captive portal URI");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap_netif),
                        TAG, "restart AP DHCP server");

    dns_server_config_t dns_config =
        DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns_server = start_dns_server(&dns_config);
    ESP_RETURN_ON_FALSE(dns_server, ESP_FAIL, TAG, "start captive DNS");

    app_settings_t settings;
    settings_store_get(&settings);
    station_configured = settings.ssid[0] != '\0';
    strlcpy(station_ssid, settings.ssid, sizeof(station_ssid));
    if (station_configured) {
        ESP_RETURN_ON_ERROR(wifi_manager_apply_station(),
                            TAG, "connect station");
    }
    ESP_LOGI(TAG, "setup AP '%s' at http://192.168.4.1",
             CONFIG_CHAT_AP_SSID);
    status_led_set(station_configured ? STATUS_LED_CONNECTING : STATUS_LED_AP_ACTIVE);
    return ESP_OK;
}

bool wifi_manager_station_connected(void)
{
    return station_connected;
}

void wifi_manager_station_ip(char *buffer, size_t buffer_size)
{
    if (buffer && buffer_size) {
        strlcpy(buffer, station_ip, buffer_size);
    }
}

void wifi_manager_station_ssid(char *buffer, size_t buffer_size)
{
    if (buffer && buffer_size) {
        strlcpy(buffer, station_ssid, buffer_size);
    }
}

int wifi_manager_station_rssi(void)
{
    wifi_ap_record_t record;
    if (!station_connected || esp_wifi_sta_get_ap_info(&record) != ESP_OK) {
        return 0;
    }
    return record.rssi;
}

int wifi_manager_scan(wifi_scan_ap_t *results, int max_results)
{
    if (!results || max_results <= 0) {
        return 0;
    }
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        if (mode != WIFI_MODE_APSTA && mode != WIFI_MODE_STA) {
            esp_wifi_set_mode(WIFI_MODE_APSTA);
        }
        esp_wifi_scan_start(&scan_config, true);
    }
    uint16_t count = 0;
    uint16_t limit = max_results > WIFI_SCAN_MAX_AP ? WIFI_SCAN_MAX_AP : max_results;
    wifi_ap_record_t records[WIFI_SCAN_MAX_AP];
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_records(&limit, records));
    for (int i = 0; i < limit && count < max_results; i++) {
        if (records[i].ssid[0] == '\0') continue;
        strlcpy(results[count].ssid, (const char *)records[i].ssid, 33);
        results[count].rssi = records[i].rssi;
        results[count].secure = (records[i].authmode != WIFI_AUTH_OPEN);
        count++;
    }
    ESP_LOGI(TAG, "scan found %d visible networks", count);
    return count;
}
