#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/temperature_sensor.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/task.h"
#include "repeater_settings.h"

typedef struct {
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;
    httpd_handle_t http_server;
    TaskHandle_t status_task_handle;
    TaskHandle_t internet_probe_task_handle;
    TaskHandle_t failsafe_reboot_task_handle;
    int retry_count;
    int upstream_probe_failure_count;
    int ap_client_count;
    int64_t upstream_connected_at_epoch;
    int64_t upstream_connected_monotonic_us;
    int64_t upstream_unavailable_since_monotonic_us;
    int64_t no_client_since_monotonic_us;
    char softap_mac[REPEATER_MAC_STRING_LEN];
    bool sta_has_ip_address;
    bool sta_has_upstream_connection;
    bool sta_using_backup_connection;
    bool napt_enabled;
    bool packet_stats_enabled;
    temperature_sensor_handle_t temp_sensor;
    bool started;
} repeater_runtime_t;

