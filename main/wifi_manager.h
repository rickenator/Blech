#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define WIFI_SCAN_MAX_AP 16

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} wifi_scan_ap_t;

esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_apply_station(void);
bool wifi_manager_station_connected(void);
void wifi_manager_station_ip(char *buffer, size_t buffer_size);
void wifi_manager_station_ssid(char *buffer, size_t buffer_size);
int wifi_manager_station_rssi(void);
int wifi_manager_scan(wifi_scan_ap_t *results, int max_results);
