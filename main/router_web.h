#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "repeater_settings.h"
#include "repeater_types.h"

typedef struct {
    const repeater_station_config_t *(*get_station_config)(void);
    const repeater_station_config_t *(*get_backup_station_config)(void);
    const repeater_softap_config_t *(*get_softap_config)(void);
    const repeater_web_auth_config_t *(*get_web_auth_config)(void);
    float (*get_chip_temperature_c)(void);
    const char *esp_mac;
    bool (*is_upstream_connected)(void);
    bool (*is_using_backup_upstream)(void);
    const char *(*get_active_upstream_ssid)(void);
    esp_err_t (*get_upstream_ip_info)(esp_netif_ip_info_t *out_ip_info);
    int64_t (*get_upstream_connected_at_epoch)(void);
    esp_err_t (*get_upstream_packet_counts)(uint32_t *out_rx_packets,
                                            uint32_t *out_tx_packets);
    int (*get_client_count)(void);
    size_t (*get_clients)(repeater_client_info_t *clients, size_t max_clients);
    const char *(*get_signal_level_label)(void);
    const char *(*get_signal_level_token)(void);
    esp_err_t (*set_signal_level_by_token)(const char *token);
    const char *(*get_theme_label)(void);
    const char *(*get_theme_token)(void);
    esp_err_t (*set_theme_by_token)(const char *token);
    const char *(*get_softap_auth_token)(void);
    repeater_auto_reboot_config_t (*get_auto_reboot_config)(void);
    esp_err_t (*set_auto_reboot_config)(bool enabled, uint8_t hour, uint8_t minute);
    esp_err_t (*set_device_description)(const char *mac, const char *description);
    esp_err_t (*set_wifi_networks)(const char *station_ssid, const char *station_password,
                                   const char *backup_station_ssid,
                                   const char *backup_station_password,
                                   const char *softap_ssid, const char *softap_password,
                                   const char *softap_auth_token);
    esp_err_t (*set_web_auth)(const char *username, const char *password);
    void (*get_firmware_status)(repeater_firmware_status_t *out_status);
    esp_err_t (*check_firmware_update)(repeater_firmware_status_t *out_status);
    esp_err_t (*start_firmware_update)(void);
    const repeater_signal_level_t *signal_levels;
    size_t signal_level_count;
    const repeater_theme_option_t *theme_options;
    size_t theme_option_count;
    const repeater_softap_auth_option_t *softap_auth_options;
    size_t softap_auth_option_count;
} router_web_context_t;

esp_err_t router_web_start(const router_web_context_t *context, httpd_handle_t *out_server);
void router_web_stop(httpd_handle_t server);
