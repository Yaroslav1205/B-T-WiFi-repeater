#pragma once

#include <stdbool.h>
#include "esp_wifi_types_generic.h"

/* Project metadata */
#define PROJECT_DEVICE_NAME                 "B-T WiFi repeater"
#define PROJECT_FIRMWARE_VERSION            "1.0.5"

/* Station configuration */
#define PROJECT_WIFI_STA_SSID                "TP-Link_8B91"
#define PROJECT_WIFI_STA_PASSWORD            "12341234"
#define PROJECT_WIFI_STA_BACKUP_ENABLED      false
#define PROJECT_WIFI_STA_BACKUP_SSID         ""
#define PROJECT_WIFI_STA_BACKUP_PASSWORD     ""
#define PROJECT_WIFI_STA_MAXIMUM_RETRY       5
#define PROJECT_WIFI_STA_FAILOVER_RETRY_THRESHOLD 3
#define PROJECT_WIFI_STA_SCAN_AUTH_MODE      WIFI_AUTH_WPA2_PSK

/* SoftAP configuration */
#define PROJECT_WIFI_AP_SSID                 "TP-Link_8B93"
#define PROJECT_WIFI_AP_PASSWORD             "12341234"
#define PROJECT_WIFI_AP_AUTH_MODE            WIFI_AUTH_WPA_WPA2_PSK
#define PROJECT_WIFI_AP_CHANNEL              1
#define PROJECT_WIFI_AP_MAX_STA_CONN         4

/* Status LED configuration */
#define PROJECT_STATUS_LED_GPIO              8
#define PROJECT_STATUS_LED_ACTIVE_LEVEL      0
#define PROJECT_STATUS_LED_ENABLED           true

/* Default Wi-Fi signal level for web-configurable TX power */
#define PROJECT_DEFAULT_TX_POWER_QUARTER_DBM 84

/* Default web theme saved in NVS on first run */
#define PROJECT_DEFAULT_WEB_THEME           "light"

/* Automatic daily reboot protection */
#define PROJECT_DEFAULT_AUTO_REBOOT_ENABLED false
#define PROJECT_DEFAULT_AUTO_REBOOT_HOUR    4
#define PROJECT_DEFAULT_AUTO_REBOOT_MINUTE  30
#define PROJECT_REBOOT_CHECK_INTERVAL_MS    10000
#define PROJECT_SNTP_SERVER                 "pool.ntp.org"
#define PROJECT_TIMEZONE_TZ                 "EET-2EEST,M3.5.0/3,M10.5.0/4"

/* Periodic diagnostic logging */
#define PROJECT_STATUS_LOG_INTERVAL_MS      10000

/* Upstream internet reachability probe */
#define PROJECT_UPSTREAM_PROBE_URL          "http://connectivitycheck.gstatic.com/generate_204"
#define PROJECT_UPSTREAM_PROBE_TIMEOUT_MS   4000
#define PROJECT_UPSTREAM_PROBE_INTERVAL_MS  15000
#define PROJECT_UPSTREAM_PROBE_RETRY_MS     5000
#define PROJECT_UPSTREAM_PROBE_FAILURE_THRESHOLD 3

/* Web UI authentication. Change these values here when needed. */
#define PROJECT_WEB_AUTH_USERNAME           "admin"
#define PROJECT_WEB_AUTH_PASSWORD           "admin"

/* Firmware update server */
#define PROJECT_FIRMWARE_MANIFEST_URL       "https://github.com/Yaroslav1205/B-T-WiFi-repeater/releases/latest/download/repeater-ota-manifest.txt"
#define PROJECT_FIRMWARE_HTTP_TIMEOUT_MS    10000
#define PROJECT_FIRMWARE_REBOOT_DELAY_MS    2000
