#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types_generic.h"

#define REPEATER_MAC_STRING_LEN 18
#define REPEATER_DEVICE_DESCRIPTION_MAX_LEN 48
#define REPEATER_WIFI_SSID_MAX_LEN 32
#define REPEATER_WIFI_PASSWORD_MAX_LEN 63
#define REPEATER_WEB_AUTH_USERNAME_MAX_LEN 32
#define REPEATER_WEB_AUTH_PASSWORD_MAX_LEN 63
#define REPEATER_CLIENT_HISTORY_MAX_ENTRIES 64

typedef struct {
    const char *token;
    const char *label;
    int8_t quarter_dbm;
} repeater_signal_level_t;

typedef struct {
    const char *token;
    const char *label;
} repeater_theme_option_t;

typedef struct {
    const char *token;
    const char *label;
    wifi_auth_mode_t auth_mode;
} repeater_softap_auth_option_t;

typedef struct {
    bool enabled;
    uint8_t hour;
    uint8_t minute;
} repeater_auto_reboot_config_t;

typedef struct {
    char ssid[REPEATER_WIFI_SSID_MAX_LEN + 1];
    char password[REPEATER_WIFI_PASSWORD_MAX_LEN + 1];
} repeater_station_config_t;

typedef struct {
    char ssid[REPEATER_WIFI_SSID_MAX_LEN + 1];
    char password[REPEATER_WIFI_PASSWORD_MAX_LEN + 1];
    wifi_auth_mode_t auth_mode;
} repeater_softap_config_t;

typedef struct {
    char username[REPEATER_WEB_AUTH_USERNAME_MAX_LEN + 1];
    char password[REPEATER_WEB_AUTH_PASSWORD_MAX_LEN + 1];
} repeater_web_auth_config_t;

typedef struct {
    char mac[REPEATER_MAC_STRING_LEN];
    char description[REPEATER_DEVICE_DESCRIPTION_MAX_LEN + 1];
} repeater_device_description_t;

typedef struct {
    char mac[REPEATER_MAC_STRING_LEN];
    char description[REPEATER_DEVICE_DESCRIPTION_MAX_LEN + 1];
    int64_t first_seen_epoch;
    int64_t last_seen_epoch;
} repeater_client_history_entry_t;

esp_err_t repeater_settings_init(void);
esp_err_t repeater_settings_apply(void);
esp_err_t repeater_settings_set_signal_level_by_token(const char *token);
esp_err_t repeater_settings_set_theme_by_token(const char *token);
esp_err_t repeater_settings_set_auto_reboot_config(bool enabled, uint8_t hour, uint8_t minute);
esp_err_t repeater_settings_record_client_connection(const char *mac);
esp_err_t repeater_settings_set_device_description(const char *mac, const char *description);
esp_err_t repeater_settings_set_wifi_networks(const char *primary_station_ssid,
                                              const char *primary_station_password,
                                              const char *backup_station_ssid,
                                              const char *backup_station_password,
                                              const char *softap_ssid,
                                              const char *softap_password,
                                              const char *softap_auth_token);
esp_err_t repeater_settings_set_web_auth(const char *username, const char *password);

const char *repeater_settings_get_signal_level_label(void);
const char *repeater_settings_get_signal_level_token(void);
const repeater_signal_level_t *repeater_settings_get_signal_levels(void);
size_t repeater_settings_get_signal_level_count(void);

const char *repeater_settings_get_theme_label(void);
const char *repeater_settings_get_theme_token(void);
const repeater_theme_option_t *repeater_settings_get_theme_options(void);
size_t repeater_settings_get_theme_option_count(void);
const char *repeater_settings_get_softap_auth_label(void);
const char *repeater_settings_get_softap_auth_token(void);
const repeater_softap_auth_option_t *repeater_settings_get_softap_auth_options(void);
size_t repeater_settings_get_softap_auth_option_count(void);

bool repeater_settings_is_auto_reboot_enabled(void);
repeater_auto_reboot_config_t repeater_settings_get_auto_reboot_config(void);
size_t repeater_settings_get_client_history(repeater_client_history_entry_t *entries, size_t max_entries);
const char *repeater_settings_get_device_description(const char *mac);
const repeater_station_config_t *repeater_settings_get_station_config(void);
const repeater_station_config_t *repeater_settings_get_backup_station_config(void);
const repeater_softap_config_t *repeater_settings_get_softap_config(void);
const repeater_web_auth_config_t *repeater_settings_get_web_auth_config(void);
