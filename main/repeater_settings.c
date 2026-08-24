#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "project_wifi_config.h"
#include "repeater_settings.h"

#define NVS_NAMESPACE    "repeater_cfg"
#define NVS_KEY_TX_POWER "tx_power"
#define NVS_KEY_THEME    "web_theme"
#define NVS_KEY_REBOOT_EN "reboot_en"
#define NVS_KEY_REBOOT_H  "reboot_h"
#define NVS_KEY_REBOOT_M  "reboot_m"
#define NVS_KEY_STA_SSID  "sta_ssid"
#define NVS_KEY_STA_PASS  "sta_pass"
#define NVS_KEY_STA_B_SSID "sta_b_ssid"
#define NVS_KEY_STA_B_PASS "sta_b_pass"
#define NVS_KEY_AP_SSID   "ap_ssid"
#define NVS_KEY_AP_PASS   "ap_pass"
#define NVS_KEY_AP_AUTH   "ap_auth"
#define NVS_KEY_WEB_USER  "web_user"
#define NVS_KEY_WEB_PASS  "web_pass"
#define NVS_KEY_DEVICE_DESCRIPTIONS "dev_descs"
#define NVS_KEY_CLIENT_HISTORY "client_hist"
#define THEME_TOKEN_MAX_LEN 16
#define LEGACY_DEVICE_DESCRIPTION_MAX_ENTRIES 24
#define DEVICE_DESCRIPTION_STORE_VERSION 1
#define CLIENT_HISTORY_STORE_VERSION 1
#define MIN_VALID_UNIX_TIMESTAMP 1704067200LL

static const char *TAG = "RepeaterCfg";

typedef struct {
    uint8_t version;
    uint8_t count;
    uint8_t reserved[2];
    repeater_device_description_t entries[LEGACY_DEVICE_DESCRIPTION_MAX_ENTRIES];
} repeater_device_description_store_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    uint8_t reserved[6];
    repeater_client_history_entry_t entries[REPEATER_CLIENT_HISTORY_MAX_ENTRIES];
} repeater_client_history_store_t;

static const repeater_signal_level_t s_signal_levels[] = {
    { "low", "Low (5 dBm)", 20 },
    { "medium", "Medium (10 dBm)", 40 },
    { "high", "High (15 dBm)", 60 },
    { "max", "Max (21 dBm)", 84 },
};

static const repeater_theme_option_t s_theme_options[] = {
    { "light", "Light theme" },
    { "dark", "Dark theme" },
};

static const repeater_softap_auth_option_t s_softap_auth_options[] = {
    { "open", "Open", WIFI_AUTH_OPEN },
    { "wpa2", "WPA2-PSK", WIFI_AUTH_WPA2_PSK },
    { "mixed", "WPA/WPA2-PSK", WIFI_AUTH_WPA_WPA2_PSK },
};

static int8_t s_tx_power_quarter_dbm = PROJECT_DEFAULT_TX_POWER_QUARTER_DBM;
static char s_theme_token[THEME_TOKEN_MAX_LEN] = PROJECT_DEFAULT_WEB_THEME;
static repeater_auto_reboot_config_t s_auto_reboot_config = {
    .enabled = PROJECT_DEFAULT_AUTO_REBOOT_ENABLED,
    .hour = PROJECT_DEFAULT_AUTO_REBOOT_HOUR,
    .minute = PROJECT_DEFAULT_AUTO_REBOOT_MINUTE,
};
static repeater_station_config_t s_station_config;
static repeater_station_config_t s_backup_station_config;
static repeater_softap_config_t s_softap_config;
static repeater_web_auth_config_t s_web_auth_config;
static repeater_client_history_entry_t s_client_history[REPEATER_CLIENT_HISTORY_MAX_ENTRIES];
static size_t s_client_history_count = 0;

static int hex_char_to_int(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static bool normalize_mac_address(const char *input, char *output, size_t output_size)
{
    uint8_t octets[6];
    size_t octet_index = 0;
    const char *cursor = input;

    if (input == NULL || output == NULL || output_size < REPEATER_MAC_STRING_LEN) {
        return false;
    }

    while (*cursor != '\0' && octet_index < 6) {
        while (*cursor == ' ' || *cursor == '\t') {
            ++cursor;
        }

        int high = hex_char_to_int(cursor[0]);
        int low = hex_char_to_int(cursor[1]);

        if (high < 0 || low < 0) {
            return false;
        }

        octets[octet_index++] = (uint8_t)((high << 4) | low);
        cursor += 2;

        if (octet_index == 6) {
            break;
        }

        if (*cursor != ':' && *cursor != '-') {
            return false;
        }

        ++cursor;
    }

    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    if (octet_index != 6 || *cursor != '\0') {
        return false;
    }

    snprintf(output, output_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             octets[0], octets[1], octets[2], octets[3], octets[4], octets[5]);
    return true;
}

static void trim_text_copy(const char *input, char *output, size_t output_size)
{
    const char *start = input != NULL ? input : "";
    const char *end = start + strlen(start);

    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    while (end > start && isspace((unsigned char)end[-1])) {
        --end;
    }

    size_t length = (size_t)(end - start);
    if (length >= output_size) {
        length = output_size - 1;
    }

    memcpy(output, start, length);
    output[length] = '\0';
}

static void copy_text(const char *input, char *output, size_t output_size)
{
    const char *value = input != NULL ? input : "";
    size_t length = strlen(value);

    if (length >= output_size) {
        length = output_size - 1;
    }

    memcpy(output, value, length);
    output[length] = '\0';
}

static void clear_client_history(void)
{
    memset(s_client_history, 0, sizeof(s_client_history));
    s_client_history_count = 0;
}

static int find_client_history_index(const char *normalized_mac)
{
    for (size_t i = 0; i < s_client_history_count; ++i) {
        if (strcmp(s_client_history[i].mac, normalized_mac) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int64_t get_current_timestamp_if_synced(void)
{
    time_t now = time(NULL);
    if ((int64_t)now < MIN_VALID_UNIX_TIMESTAMP) {
        return 0;
    }

    return (int64_t)now;
}

static void remove_client_history_entry(size_t index)
{
    if (index >= s_client_history_count) {
        return;
    }

    for (size_t i = index; i + 1 < s_client_history_count; ++i) {
        s_client_history[i] = s_client_history[i + 1];
    }

    memset(&s_client_history[s_client_history_count - 1], 0,
           sizeof(s_client_history[s_client_history_count - 1]));
    --s_client_history_count;
}

static repeater_client_history_entry_t *ensure_client_history_entry(const char *normalized_mac)
{
    int index = find_client_history_index(normalized_mac);

    if (index >= 0) {
        return &s_client_history[index];
    }

    if (s_client_history_count >= REPEATER_CLIENT_HISTORY_MAX_ENTRIES) {
        return NULL;
    }

    repeater_client_history_entry_t *entry = &s_client_history[s_client_history_count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->mac, sizeof(entry->mac), "%s", normalized_mac);
    return entry;
}

static const repeater_signal_level_t *find_signal_level_by_token(const char *token)
{
    for (size_t i = 0; i < repeater_settings_get_signal_level_count(); ++i) {
        if (strcmp(token, s_signal_levels[i].token) == 0) {
            return &s_signal_levels[i];
        }
    }

    return NULL;
}

static const repeater_signal_level_t *find_signal_level_by_tx_power(int8_t quarter_dbm)
{
    for (size_t i = 0; i < repeater_settings_get_signal_level_count(); ++i) {
        if (s_signal_levels[i].quarter_dbm == quarter_dbm) {
            return &s_signal_levels[i];
        }
    }

    return NULL;
}

static const repeater_theme_option_t *find_theme_option_by_token(const char *token)
{
    for (size_t i = 0; i < repeater_settings_get_theme_option_count(); ++i) {
        if (strcmp(token, s_theme_options[i].token) == 0) {
            return &s_theme_options[i];
        }
    }

    return NULL;
}

static const repeater_softap_auth_option_t *find_softap_auth_option_by_token(const char *token)
{
    for (size_t i = 0; i < repeater_settings_get_softap_auth_option_count(); ++i) {
        if (strcmp(token, s_softap_auth_options[i].token) == 0) {
            return &s_softap_auth_options[i];
        }
    }

    return NULL;
}

static const repeater_softap_auth_option_t *find_softap_auth_option_by_mode(wifi_auth_mode_t auth_mode)
{
    for (size_t i = 0; i < repeater_settings_get_softap_auth_option_count(); ++i) {
        if (s_softap_auth_options[i].auth_mode == auth_mode) {
            return &s_softap_auth_options[i];
        }
    }

    return NULL;
}

static void set_default_theme_token(void)
{
    snprintf(s_theme_token, sizeof(s_theme_token), "%s", PROJECT_DEFAULT_WEB_THEME);
}

static void set_default_auto_reboot_config(void)
{
    s_auto_reboot_config.enabled = PROJECT_DEFAULT_AUTO_REBOOT_ENABLED;
    s_auto_reboot_config.hour = PROJECT_DEFAULT_AUTO_REBOOT_HOUR;
    s_auto_reboot_config.minute = PROJECT_DEFAULT_AUTO_REBOOT_MINUTE;
}

static void set_default_station_config(void)
{
    copy_text(PROJECT_WIFI_STA_SSID, s_station_config.ssid, sizeof(s_station_config.ssid));
    copy_text(PROJECT_WIFI_STA_PASSWORD, s_station_config.password, sizeof(s_station_config.password));
}

static void set_default_backup_station_config(void)
{
    copy_text(PROJECT_WIFI_STA_BACKUP_SSID, s_backup_station_config.ssid,
              sizeof(s_backup_station_config.ssid));
    copy_text(PROJECT_WIFI_STA_BACKUP_PASSWORD, s_backup_station_config.password,
              sizeof(s_backup_station_config.password));
}

static void set_default_softap_config(void)
{
    copy_text(PROJECT_WIFI_AP_SSID, s_softap_config.ssid, sizeof(s_softap_config.ssid));
    copy_text(PROJECT_WIFI_AP_PASSWORD, s_softap_config.password, sizeof(s_softap_config.password));
    s_softap_config.auth_mode = PROJECT_WIFI_AP_AUTH_MODE;
}

static void set_default_web_auth_config(void)
{
    copy_text(PROJECT_WEB_AUTH_USERNAME, s_web_auth_config.username,
              sizeof(s_web_auth_config.username));
    copy_text(PROJECT_WEB_AUTH_PASSWORD, s_web_auth_config.password,
              sizeof(s_web_auth_config.password));
}

static esp_err_t load_client_history_from_store(const repeater_client_history_store_t *store)
{
    clear_client_history();

    if (store->version != CLIENT_HISTORY_STORE_VERSION ||
        store->count > REPEATER_CLIENT_HISTORY_MAX_ENTRIES) {
        return ESP_ERR_INVALID_VERSION;
    }

    for (size_t i = 0; i < store->count; ++i) {
        char normalized_mac[REPEATER_MAC_STRING_LEN];
        char trimmed_description[REPEATER_DEVICE_DESCRIPTION_MAX_LEN + 1];
        repeater_client_history_entry_t *entry;
        int64_t first_seen = store->entries[i].first_seen_epoch;
        int64_t last_seen = store->entries[i].last_seen_epoch;

        if (!normalize_mac_address(store->entries[i].mac, normalized_mac, sizeof(normalized_mac))) {
            return ESP_ERR_INVALID_ARG;
        }

        trim_text_copy(store->entries[i].description, trimmed_description, sizeof(trimmed_description));
        if (first_seen < MIN_VALID_UNIX_TIMESTAMP) {
            first_seen = 0;
        }
        if (last_seen < MIN_VALID_UNIX_TIMESTAMP) {
            last_seen = 0;
        }
        if (first_seen == 0 && last_seen != 0) {
            first_seen = last_seen;
        }
        if (last_seen != 0 && first_seen != 0 && last_seen < first_seen) {
            last_seen = first_seen;
        }

        entry = ensure_client_history_entry(normalized_mac);
        if (entry == NULL) {
            return ESP_ERR_NO_MEM;
        }

        snprintf(entry->description, sizeof(entry->description), "%s", trimmed_description);
        entry->first_seen_epoch = first_seen;
        entry->last_seen_epoch = last_seen;
    }

    return ESP_OK;
}

static esp_err_t load_legacy_device_descriptions_from_store(const repeater_device_description_store_t *store)
{
    clear_client_history();

    if (store->version != DEVICE_DESCRIPTION_STORE_VERSION ||
        store->count > LEGACY_DEVICE_DESCRIPTION_MAX_ENTRIES) {
        return ESP_ERR_INVALID_VERSION;
    }

    for (size_t i = 0; i < store->count; ++i) {
        char normalized_mac[REPEATER_MAC_STRING_LEN];
        char trimmed_description[REPEATER_DEVICE_DESCRIPTION_MAX_LEN + 1];
        repeater_client_history_entry_t *entry;

        if (!normalize_mac_address(store->entries[i].mac, normalized_mac, sizeof(normalized_mac))) {
            return ESP_ERR_INVALID_ARG;
        }

        trim_text_copy(store->entries[i].description, trimmed_description, sizeof(trimmed_description));
        if (trimmed_description[0] == '\0') {
            continue;
        }

        entry = ensure_client_history_entry(normalized_mac);
        if (entry == NULL) {
            return ESP_ERR_NO_MEM;
        }

        snprintf(entry->description, sizeof(entry->description), "%s", trimmed_description);
    }

    return ESP_OK;
}

static bool is_valid_auto_reboot_time(uint8_t hour, uint8_t minute)
{
    return hour < 24 && minute < 60;
}

static bool is_valid_wifi_ssid(const char *ssid)
{
    size_t length = ssid != NULL ? strlen(ssid) : 0;
    return length > 0 && length <= REPEATER_WIFI_SSID_MAX_LEN;
}

static bool is_valid_wifi_password(const char *password)
{
    size_t length = password != NULL ? strlen(password) : 0;
    return length == 0 || (length >= 8 && length <= REPEATER_WIFI_PASSWORD_MAX_LEN);
}

static bool is_valid_optional_wifi_config(const char *ssid, const char *password)
{
    size_t ssid_length = ssid != NULL ? strlen(ssid) : 0;

    if (ssid_length == 0) {
        return password == NULL || password[0] == '\0';
    }

    return is_valid_wifi_ssid(ssid) && is_valid_wifi_password(password);
}

static bool is_valid_softap_auth_password(wifi_auth_mode_t auth_mode, const char *password)
{
    if (auth_mode == WIFI_AUTH_OPEN) {
        return password != NULL;
    }

    return is_valid_wifi_password(password) && password != NULL && password[0] != '\0';
}

static bool is_valid_web_auth_username(const char *username)
{
    size_t length = username != NULL ? strlen(username) : 0;
    return length > 0 && length <= REPEATER_WEB_AUTH_USERNAME_MAX_LEN;
}

static bool is_valid_web_auth_password(const char *password)
{
    size_t length = password != NULL ? strlen(password) : 0;
    return length > 0 && length <= REPEATER_WEB_AUTH_PASSWORD_MAX_LEN;
}

static esp_err_t save_client_history_to_nvs(nvs_handle_t nvs_handle)
{
    repeater_client_history_store_t *store = calloc(1, sizeof(*store));
    esp_err_t err;

    ESP_RETURN_ON_FALSE(store != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate client history save buffer");

    store->version = CLIENT_HISTORY_STORE_VERSION;
    store->count = (uint8_t)s_client_history_count;

    for (size_t i = 0; i < s_client_history_count; ++i) {
        store->entries[i] = s_client_history[i];
    }

    err = nvs_set_blob(nvs_handle, NVS_KEY_CLIENT_HISTORY, store, sizeof(*store));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    free(store);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to save client history to NVS");
    return ESP_OK;
}

static esp_err_t save_tx_power_to_nvs(nvs_handle_t nvs_handle, int8_t quarter_dbm)
{
    esp_err_t err = nvs_set_i8(nvs_handle, NVS_KEY_TX_POWER, quarter_dbm);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to save TX power to NVS");
    return ESP_OK;
}

static esp_err_t save_theme_to_nvs(nvs_handle_t nvs_handle, const char *token)
{
    esp_err_t err = nvs_set_str(nvs_handle, NVS_KEY_THEME, token);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to save theme to NVS");
    return ESP_OK;
}

static esp_err_t save_auto_reboot_to_nvs(nvs_handle_t nvs_handle, bool enabled,
                                         uint8_t hour, uint8_t minute)
{
    esp_err_t err = nvs_set_u8(nvs_handle, NVS_KEY_REBOOT_EN, enabled ? 1 : 0);

    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_REBOOT_H, hour);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_REBOOT_M, minute);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to save auto reboot config to NVS");
    return ESP_OK;
}

static esp_err_t save_wifi_networks_to_nvs(nvs_handle_t nvs_handle,
                                           const repeater_station_config_t *station_config,
                                           const repeater_station_config_t *backup_station_config,
                                           const repeater_softap_config_t *softap_config)
{
    esp_err_t err = nvs_set_str(nvs_handle, NVS_KEY_STA_SSID, station_config->ssid);

    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_STA_PASS, station_config->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_STA_B_SSID, backup_station_config->ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_STA_B_PASS, backup_station_config->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, softap_config->ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_AP_PASS, softap_config->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, NVS_KEY_AP_AUTH, (uint8_t)softap_config->auth_mode);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to save Wi-Fi network settings to NVS");
    return ESP_OK;
}

static esp_err_t save_web_auth_to_nvs(nvs_handle_t nvs_handle,
                                      const repeater_web_auth_config_t *web_auth_config)
{
    esp_err_t err = nvs_set_str(nvs_handle, NVS_KEY_WEB_USER, web_auth_config->username);

    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, NVS_KEY_WEB_PASS, web_auth_config->password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to save web auth settings to NVS");
    return ESP_OK;
}

esp_err_t repeater_settings_init(void)
{
    nvs_handle_t nvs_handle;
    int8_t stored_value = PROJECT_DEFAULT_TX_POWER_QUARTER_DBM;
    size_t theme_token_len = sizeof(s_theme_token);
    uint8_t reboot_enabled = PROJECT_DEFAULT_AUTO_REBOOT_ENABLED ? 1 : 0;
    uint8_t reboot_hour = PROJECT_DEFAULT_AUTO_REBOOT_HOUR;
    uint8_t reboot_minute = PROJECT_DEFAULT_AUTO_REBOOT_MINUTE;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_tx_power_quarter_dbm = PROJECT_DEFAULT_TX_POWER_QUARTER_DBM;
        set_default_theme_token();
        set_default_auto_reboot_config();
        set_default_station_config();
        set_default_backup_station_config();
        set_default_softap_config();
        set_default_web_auth_config();
        clear_client_history();
        ESP_LOGI(TAG, "No saved signal level in NVS, using default: %s",
                 repeater_settings_get_signal_level_label());
        ESP_LOGI(TAG, "No saved theme in NVS, using default: %s",
                 repeater_settings_get_theme_label());
        ESP_LOGI(TAG, "No saved auto reboot config in NVS, using default: %s at %02u:%02u",
                 s_auto_reboot_config.enabled ? "enabled" : "disabled",
                 s_auto_reboot_config.hour, s_auto_reboot_config.minute);
        ESP_LOGI(TAG, "No saved station config in NVS, using default SSID: %s",
                 s_station_config.ssid);
        ESP_LOGI(TAG, "No saved backup station config in NVS, using default SSID: %s",
                 s_backup_station_config.ssid[0] != '\0' ? s_backup_station_config.ssid : "(disabled)");
        ESP_LOGI(TAG, "No saved SoftAP config in NVS, using default SSID: %s",
                 s_softap_config.ssid);
        ESP_LOGI(TAG, "No saved web auth config in NVS, using default username: %s",
                 s_web_auth_config.username);
        ESP_LOGI(TAG, "No saved client history in NVS");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to open NVS for reading");

    err = nvs_get_i8(nvs_handle, NVS_KEY_TX_POWER, &stored_value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_tx_power_quarter_dbm = PROJECT_DEFAULT_TX_POWER_QUARTER_DBM;
        ESP_LOGI(TAG, "Signal level key not found in NVS, using default: %s",
                 repeater_settings_get_signal_level_label());
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to read TX power from NVS");

        if (find_signal_level_by_tx_power(stored_value) == NULL) {
            s_tx_power_quarter_dbm = PROJECT_DEFAULT_TX_POWER_QUARTER_DBM;
            ESP_LOGW(TAG, "Invalid saved TX power %d, using default: %s",
                     stored_value, repeater_settings_get_signal_level_label());
        } else {
            s_tx_power_quarter_dbm = stored_value;
            ESP_LOGI(TAG, "Loaded signal level from NVS: %s",
                     repeater_settings_get_signal_level_label());
        }
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_THEME, s_theme_token, &theme_token_len);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_default_theme_token();
        ESP_LOGI(TAG, "Theme key not found in NVS, using default: %s",
                 repeater_settings_get_theme_label());
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to read theme from NVS");

        if (find_theme_option_by_token(s_theme_token) == NULL) {
            set_default_theme_token();
            ESP_LOGW(TAG, "Invalid saved theme, using default: %s",
                     repeater_settings_get_theme_label());
        } else {
            ESP_LOGI(TAG, "Loaded theme from NVS: %s", repeater_settings_get_theme_label());
        }
    }

    err = nvs_get_u8(nvs_handle, NVS_KEY_REBOOT_EN, &reboot_enabled);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_default_auto_reboot_config();
        ESP_LOGI(TAG, "Auto reboot enable key not found in NVS, using default: %s",
                 s_auto_reboot_config.enabled ? "enabled" : "disabled");
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to read auto reboot enable from NVS");

        err = nvs_get_u8(nvs_handle, NVS_KEY_REBOOT_H, &reboot_hour);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            set_default_auto_reboot_config();
            ESP_LOGI(TAG, "Auto reboot hour key not found in NVS, using default: %02u:%02u",
                     s_auto_reboot_config.hour, s_auto_reboot_config.minute);
        } else {
            ESP_RETURN_ON_ERROR(err, TAG, "Failed to read auto reboot hour from NVS");

            err = nvs_get_u8(nvs_handle, NVS_KEY_REBOOT_M, &reboot_minute);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                set_default_auto_reboot_config();
                ESP_LOGI(TAG, "Auto reboot minute key not found in NVS, using default: %02u:%02u",
                         s_auto_reboot_config.hour, s_auto_reboot_config.minute);
            } else {
                ESP_RETURN_ON_ERROR(err, TAG, "Failed to read auto reboot minute from NVS");

                if (!is_valid_auto_reboot_time(reboot_hour, reboot_minute)) {
                    set_default_auto_reboot_config();
                    ESP_LOGW(TAG, "Invalid saved auto reboot time, using default: %02u:%02u",
                             s_auto_reboot_config.hour, s_auto_reboot_config.minute);
                } else {
                    s_auto_reboot_config.enabled = reboot_enabled != 0;
                    s_auto_reboot_config.hour = reboot_hour;
                    s_auto_reboot_config.minute = reboot_minute;
                    ESP_LOGI(TAG, "Loaded auto reboot config: %s at %02u:%02u",
                             s_auto_reboot_config.enabled ? "enabled" : "disabled",
                             s_auto_reboot_config.hour, s_auto_reboot_config.minute);
                }
            }
        }
    }

    size_t station_ssid_len = sizeof(s_station_config.ssid);
    err = nvs_get_str(nvs_handle, NVS_KEY_STA_SSID, s_station_config.ssid, &station_ssid_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_default_station_config();
        ESP_LOGI(TAG, "Station SSID key not found in NVS, using default: %s",
                 s_station_config.ssid);
    } else if (err != ESP_OK || !is_valid_wifi_ssid(s_station_config.ssid)) {
        set_default_station_config();
        ESP_LOGW(TAG, "Invalid saved station SSID, using default: %s",
                 s_station_config.ssid);
    } else {
        size_t station_password_len = sizeof(s_station_config.password);
        err = nvs_get_str(nvs_handle, NVS_KEY_STA_PASS, s_station_config.password,
                          &station_password_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            copy_text(PROJECT_WIFI_STA_PASSWORD, s_station_config.password,
                      sizeof(s_station_config.password));
            ESP_LOGI(TAG, "Station password key not found in NVS, using default password");
        } else if (err != ESP_OK || !is_valid_wifi_password(s_station_config.password)) {
            set_default_station_config();
            ESP_LOGW(TAG, "Invalid saved station password, using default credentials");
        } else {
            ESP_LOGI(TAG, "Loaded station config from NVS: SSID=%s", s_station_config.ssid);
        }
    }

    size_t backup_station_ssid_len = sizeof(s_backup_station_config.ssid);
    err = nvs_get_str(nvs_handle, NVS_KEY_STA_B_SSID, s_backup_station_config.ssid,
                      &backup_station_ssid_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_default_backup_station_config();
        ESP_LOGI(TAG, "Backup station SSID key not found in NVS, using default: %s",
                 s_backup_station_config.ssid[0] != '\0' ? s_backup_station_config.ssid : "(disabled)");
    } else if (err != ESP_OK || (s_backup_station_config.ssid[0] != '\0' &&
                                 !is_valid_wifi_ssid(s_backup_station_config.ssid))) {
        set_default_backup_station_config();
        ESP_LOGW(TAG, "Invalid saved backup station SSID, using default: %s",
                 s_backup_station_config.ssid[0] != '\0' ? s_backup_station_config.ssid : "(disabled)");
    } else {
        size_t backup_station_password_len = sizeof(s_backup_station_config.password);
        err = nvs_get_str(nvs_handle, NVS_KEY_STA_B_PASS, s_backup_station_config.password,
                          &backup_station_password_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            copy_text(PROJECT_WIFI_STA_BACKUP_PASSWORD, s_backup_station_config.password,
                      sizeof(s_backup_station_config.password));
            ESP_LOGI(TAG, "Backup station password key not found in NVS, using default password");
        } else if (err != ESP_OK ||
                   !is_valid_optional_wifi_config(s_backup_station_config.ssid,
                                                  s_backup_station_config.password)) {
            set_default_backup_station_config();
            ESP_LOGW(TAG, "Invalid saved backup station password, using default credentials");
        } else {
            ESP_LOGI(TAG, "Loaded backup station config from NVS: SSID=%s",
                     s_backup_station_config.ssid[0] != '\0' ? s_backup_station_config.ssid : "(disabled)");
        }
    }

    size_t softap_ssid_len = sizeof(s_softap_config.ssid);
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, s_softap_config.ssid, &softap_ssid_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_default_softap_config();
        ESP_LOGI(TAG, "SoftAP SSID key not found in NVS, using default: %s",
                 s_softap_config.ssid);
    } else if (err != ESP_OK || !is_valid_wifi_ssid(s_softap_config.ssid)) {
        set_default_softap_config();
        ESP_LOGW(TAG, "Invalid saved SoftAP SSID, using default: %s",
                 s_softap_config.ssid);
    } else {
        size_t softap_password_len = sizeof(s_softap_config.password);
        err = nvs_get_str(nvs_handle, NVS_KEY_AP_PASS, s_softap_config.password,
                          &softap_password_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            copy_text(PROJECT_WIFI_AP_PASSWORD, s_softap_config.password,
                      sizeof(s_softap_config.password));
            ESP_LOGI(TAG, "SoftAP password key not found in NVS, using default password");
        } else if (err != ESP_OK || !is_valid_wifi_password(s_softap_config.password)) {
            set_default_softap_config();
            ESP_LOGW(TAG, "Invalid saved SoftAP password, using default credentials");
        } else {
            uint8_t softap_auth_mode = (uint8_t)PROJECT_WIFI_AP_AUTH_MODE;
            err = nvs_get_u8(nvs_handle, NVS_KEY_AP_AUTH, &softap_auth_mode);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                s_softap_config.auth_mode = PROJECT_WIFI_AP_AUTH_MODE;
                ESP_LOGI(TAG, "SoftAP auth key not found in NVS, using default mode");
            } else if (err != ESP_OK ||
                       find_softap_auth_option_by_mode((wifi_auth_mode_t)softap_auth_mode) == NULL ||
                       !is_valid_softap_auth_password((wifi_auth_mode_t)softap_auth_mode,
                                                      s_softap_config.password)) {
                set_default_softap_config();
                ESP_LOGW(TAG, "Invalid saved SoftAP auth mode, using default credentials");
            } else {
                s_softap_config.auth_mode = (wifi_auth_mode_t)softap_auth_mode;
                ESP_LOGI(TAG, "Loaded SoftAP config from NVS: SSID=%s mode=%s",
                         s_softap_config.ssid, repeater_settings_get_softap_auth_label());
            }
        }
    }

    size_t web_username_len = sizeof(s_web_auth_config.username);
    err = nvs_get_str(nvs_handle, NVS_KEY_WEB_USER, s_web_auth_config.username, &web_username_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_default_web_auth_config();
        ESP_LOGI(TAG, "Web auth username key not found in NVS, using default username: %s",
                 s_web_auth_config.username);
    } else if (err != ESP_OK || !is_valid_web_auth_username(s_web_auth_config.username)) {
        set_default_web_auth_config();
        ESP_LOGW(TAG, "Invalid saved web auth username, using default credentials");
    } else {
        size_t web_password_len = sizeof(s_web_auth_config.password);
        err = nvs_get_str(nvs_handle, NVS_KEY_WEB_PASS, s_web_auth_config.password,
                          &web_password_len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            copy_text(PROJECT_WEB_AUTH_PASSWORD, s_web_auth_config.password,
                      sizeof(s_web_auth_config.password));
            ESP_LOGI(TAG, "Web auth password key not found in NVS, using default password");
        } else if (err != ESP_OK || !is_valid_web_auth_password(s_web_auth_config.password)) {
            set_default_web_auth_config();
            ESP_LOGW(TAG, "Invalid saved web auth password, using default credentials");
        } else {
            ESP_LOGI(TAG, "Loaded web auth config from NVS: username=%s",
                     s_web_auth_config.username);
        }
    }

    {
        repeater_client_history_store_t *client_history_store = calloc(1, sizeof(*client_history_store));
        repeater_device_description_store_t *legacy_device_store = calloc(1, sizeof(*legacy_device_store));
        size_t client_history_store_size = sizeof(*client_history_store);
        size_t legacy_device_store_size = sizeof(*legacy_device_store);

        if (client_history_store == NULL || legacy_device_store == NULL) {
            free(client_history_store);
            free(legacy_device_store);
            nvs_close(nvs_handle);
            ESP_LOGE(TAG, "Failed to allocate client history load buffers");
            return ESP_ERR_NO_MEM;
        }

        err = nvs_get_blob(nvs_handle, NVS_KEY_CLIENT_HISTORY, client_history_store,
                           &client_history_store_size);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_get_blob(nvs_handle, NVS_KEY_DEVICE_DESCRIPTIONS, legacy_device_store,
                               &legacy_device_store_size);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                clear_client_history();
                ESP_LOGI(TAG, "Client history key not found in NVS");
            } else if (err == ESP_OK) {
                if (legacy_device_store_size != sizeof(*legacy_device_store) ||
                    load_legacy_device_descriptions_from_store(legacy_device_store) != ESP_OK) {
                    clear_client_history();
                    ESP_LOGW(TAG, "Invalid legacy device descriptions, ignoring stored data");
                } else {
                    ESP_LOGI(TAG, "Loaded %u legacy device descriptions into client history",
                             (unsigned int)s_client_history_count);
                }
            } else {
                free(client_history_store);
                free(legacy_device_store);
                nvs_close(nvs_handle);
                ESP_RETURN_ON_ERROR(err, TAG, "Failed to read legacy device descriptions from NVS");
            }
        } else if (err == ESP_OK) {
            if (client_history_store_size != sizeof(*client_history_store) ||
                load_client_history_from_store(client_history_store) != ESP_OK) {
                clear_client_history();
                ESP_LOGW(TAG, "Invalid saved client history, ignoring stored data");
            } else {
                ESP_LOGI(TAG, "Loaded %u saved client history records from NVS",
                         (unsigned int)s_client_history_count);
            }
        } else {
            free(client_history_store);
            free(legacy_device_store);
            nvs_close(nvs_handle);
            ESP_RETURN_ON_ERROR(err, TAG, "Failed to read client history from NVS");
        }

        free(client_history_store);
        free(legacy_device_store);
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

esp_err_t repeater_settings_apply(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_set_max_tx_power(s_tx_power_quarter_dbm), TAG,
                        "Failed to apply TX power");
    ESP_LOGI(TAG, "Applied signal level: %s", repeater_settings_get_signal_level_label());
    return ESP_OK;
}

esp_err_t repeater_settings_set_signal_level_by_token(const char *token)
{
    nvs_handle_t nvs_handle;
    const repeater_signal_level_t *level = find_signal_level_by_token(token);

    if (level == NULL) {
        ESP_LOGE(TAG, "Unsupported signal level token: %s", token);
        return ESP_ERR_INVALID_ARG;
    }

    s_tx_power_quarter_dbm = level->quarter_dbm;
    ESP_RETURN_ON_ERROR(repeater_settings_apply(), TAG, "Failed to apply new signal level");
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "Failed to open NVS for signal level write");
    esp_err_t err = save_tx_power_to_nvs(nvs_handle, level->quarter_dbm);
    nvs_close(nvs_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to persist new signal level");

    ESP_LOGI(TAG, "Saved signal level: %s", level->label);
    return ESP_OK;
}

esp_err_t repeater_settings_set_theme_by_token(const char *token)
{
    nvs_handle_t nvs_handle;
    const repeater_theme_option_t *theme = find_theme_option_by_token(token);

    if (theme == NULL) {
        ESP_LOGE(TAG, "Unsupported theme token: %s", token);
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(s_theme_token, sizeof(s_theme_token), "%s", theme->token);

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "Failed to open NVS for theme write");
    esp_err_t err = save_theme_to_nvs(nvs_handle, s_theme_token);
    nvs_close(nvs_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to persist theme");

    ESP_LOGI(TAG, "Saved web theme: %s", theme->label);
    return ESP_OK;
}

esp_err_t repeater_settings_set_auto_reboot_config(bool enabled, uint8_t hour, uint8_t minute)
{
    nvs_handle_t nvs_handle;

    if (!is_valid_auto_reboot_time(hour, minute)) {
        ESP_LOGE(TAG, "Invalid auto reboot time: %02u:%02u", hour, minute);
        return ESP_ERR_INVALID_ARG;
    }

    s_auto_reboot_config.enabled = enabled;
    s_auto_reboot_config.hour = hour;
    s_auto_reboot_config.minute = minute;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "Failed to open NVS for auto reboot write");
    esp_err_t err = save_auto_reboot_to_nvs(nvs_handle, enabled, hour, minute);
    nvs_close(nvs_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to persist auto reboot config");

    ESP_LOGI(TAG, "Saved auto reboot config: %s at %02u:%02u",
             enabled ? "enabled" : "disabled", hour, minute);
    return ESP_OK;
}

esp_err_t repeater_settings_record_client_connection(const char *mac)
{
    nvs_handle_t nvs_handle;
    char normalized_mac[REPEATER_MAC_STRING_LEN];
    repeater_client_history_entry_t *entry;
    int64_t connected_at;

    if (!normalize_mac_address(mac, normalized_mac, sizeof(normalized_mac))) {
        ESP_LOGE(TAG, "Invalid MAC for history: %s", mac != NULL ? mac : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    entry = ensure_client_history_entry(normalized_mac);
    if (entry == NULL) {
        ESP_LOGE(TAG, "Client history limit reached (%d)", REPEATER_CLIENT_HISTORY_MAX_ENTRIES);
        return ESP_ERR_NO_MEM;
    }

    connected_at = get_current_timestamp_if_synced();
    if (entry->first_seen_epoch == 0 && connected_at != 0) {
        entry->first_seen_epoch = connected_at;
    }
    if (connected_at != 0) {
        entry->last_seen_epoch = connected_at;
    }

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "Failed to open NVS for client history write");
    esp_err_t err = save_client_history_to_nvs(nvs_handle);
    nvs_close(nvs_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to persist client history");

    ESP_LOGI(TAG, "Recorded client connection for MAC %s", normalized_mac);
    return ESP_OK;
}

esp_err_t repeater_settings_set_device_description(const char *mac, const char *description)
{
    nvs_handle_t nvs_handle;
    char normalized_mac[REPEATER_MAC_STRING_LEN];
    char trimmed_description[REPEATER_DEVICE_DESCRIPTION_MAX_LEN + 1];
    repeater_client_history_entry_t *entry;

    if (!normalize_mac_address(mac, normalized_mac, sizeof(normalized_mac))) {
        ESP_LOGE(TAG, "Invalid MAC for description: %s", mac != NULL ? mac : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    trim_text_copy(description, trimmed_description, sizeof(trimmed_description));
    entry = ensure_client_history_entry(normalized_mac);

    if (entry == NULL) {
        ESP_LOGE(TAG, "Client history limit reached (%d)", REPEATER_CLIENT_HISTORY_MAX_ENTRIES);
        return ESP_ERR_NO_MEM;
    }

    snprintf(entry->description, sizeof(entry->description), "%s", trimmed_description);

    if (trimmed_description[0] == '\0' &&
        entry->first_seen_epoch == 0 &&
        entry->last_seen_epoch == 0) {
        int index = find_client_history_index(normalized_mac);
        if (index >= 0) {
            remove_client_history_entry((size_t)index);
        }
    }

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "Failed to open NVS for device description write");
    esp_err_t err = save_client_history_to_nvs(nvs_handle);
    nvs_close(nvs_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to persist client history");

    if (trimmed_description[0] == '\0') {
        ESP_LOGI(TAG, "Removed device description for MAC %s", normalized_mac);
    } else {
        ESP_LOGI(TAG, "Saved device description for MAC %s: %s",
                 normalized_mac, trimmed_description);
    }

    return ESP_OK;
}

esp_err_t repeater_settings_set_wifi_networks(const char *station_ssid,
                                              const char *station_password,
                                              const char *backup_station_ssid,
                                              const char *backup_station_password,
                                              const char *softap_ssid,
                                              const char *softap_password,
                                              const char *softap_auth_token)
{
    nvs_handle_t nvs_handle;
    repeater_station_config_t new_station_config;
    repeater_station_config_t new_backup_station_config;
    repeater_softap_config_t new_softap_config;
    const repeater_station_config_t old_station_config = s_station_config;
    const repeater_station_config_t old_backup_station_config = s_backup_station_config;
    const repeater_softap_config_t old_softap_config = s_softap_config;
    const repeater_softap_auth_option_t *softap_auth_option =
        find_softap_auth_option_by_token(softap_auth_token);

    if (!is_valid_wifi_ssid(station_ssid) || !is_valid_wifi_password(station_password) ||
        !is_valid_optional_wifi_config(backup_station_ssid, backup_station_password) ||
        !is_valid_wifi_ssid(softap_ssid) || softap_auth_option == NULL ||
        !is_valid_softap_auth_password(softap_auth_option->auth_mode, softap_password)) {
        ESP_LOGE(TAG, "Invalid Station/SoftAP parameters received");
        return ESP_ERR_INVALID_ARG;
    }

    copy_text(station_ssid, new_station_config.ssid, sizeof(new_station_config.ssid));
    copy_text(station_password, new_station_config.password, sizeof(new_station_config.password));
    copy_text(backup_station_ssid, new_backup_station_config.ssid,
              sizeof(new_backup_station_config.ssid));
    copy_text(backup_station_password, new_backup_station_config.password,
              sizeof(new_backup_station_config.password));
    copy_text(softap_ssid, new_softap_config.ssid, sizeof(new_softap_config.ssid));
    copy_text(softap_password, new_softap_config.password, sizeof(new_softap_config.password));
    new_softap_config.auth_mode = softap_auth_option->auth_mode;

    s_station_config = new_station_config;
    s_backup_station_config = new_backup_station_config;
    s_softap_config = new_softap_config;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "Failed to open NVS for Wi-Fi settings write");
    esp_err_t err = save_wifi_networks_to_nvs(nvs_handle, &s_station_config,
                                              &s_backup_station_config, &s_softap_config);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        s_station_config = old_station_config;
        s_backup_station_config = old_backup_station_config;
        s_softap_config = old_softap_config;
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to persist Wi-Fi network settings");
    }

    ESP_LOGI(TAG, "Saved uplink and SoftAP settings: primary=%s backup=%s softap=%s mode=%s",
             s_station_config.ssid,
             s_backup_station_config.ssid[0] != '\0' ? s_backup_station_config.ssid : "(disabled)",
             s_softap_config.ssid, repeater_settings_get_softap_auth_label());
    return ESP_OK;
}

esp_err_t repeater_settings_set_web_auth(const char *username, const char *password)
{
    nvs_handle_t nvs_handle;
    repeater_web_auth_config_t new_web_auth_config;
    const repeater_web_auth_config_t old_web_auth_config = s_web_auth_config;

    if (!is_valid_web_auth_username(username) || !is_valid_web_auth_password(password)) {
        ESP_LOGE(TAG, "Invalid web auth parameters received");
        return ESP_ERR_INVALID_ARG;
    }

    copy_text(username, new_web_auth_config.username, sizeof(new_web_auth_config.username));
    copy_text(password, new_web_auth_config.password, sizeof(new_web_auth_config.password));
    s_web_auth_config = new_web_auth_config;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "Failed to open NVS for web auth write");
    esp_err_t err = save_web_auth_to_nvs(nvs_handle, &s_web_auth_config);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        s_web_auth_config = old_web_auth_config;
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to persist web auth settings");
    }

    ESP_LOGI(TAG, "Saved web auth settings: username=%s", s_web_auth_config.username);
    return ESP_OK;
}

const char *repeater_settings_get_signal_level_label(void)
{
    const repeater_signal_level_t *level = find_signal_level_by_tx_power(s_tx_power_quarter_dbm);
    return level != NULL ? level->label : "Custom";
}

const char *repeater_settings_get_signal_level_token(void)
{
    const repeater_signal_level_t *level = find_signal_level_by_tx_power(s_tx_power_quarter_dbm);
    return level != NULL ? level->token : "";
}

const repeater_signal_level_t *repeater_settings_get_signal_levels(void)
{
    return s_signal_levels;
}

size_t repeater_settings_get_signal_level_count(void)
{
    return sizeof(s_signal_levels) / sizeof(s_signal_levels[0]);
}

const char *repeater_settings_get_theme_label(void)
{
    const repeater_theme_option_t *theme = find_theme_option_by_token(s_theme_token);
    return theme != NULL ? theme->label : "Unknown theme";
}

const char *repeater_settings_get_theme_token(void)
{
    return s_theme_token;
}

const repeater_theme_option_t *repeater_settings_get_theme_options(void)
{
    return s_theme_options;
}

size_t repeater_settings_get_theme_option_count(void)
{
    return sizeof(s_theme_options) / sizeof(s_theme_options[0]);
}

const char *repeater_settings_get_softap_auth_label(void)
{
    const repeater_softap_auth_option_t *option =
        find_softap_auth_option_by_mode(s_softap_config.auth_mode);
    return option != NULL ? option->label : "Unknown";
}

const char *repeater_settings_get_softap_auth_token(void)
{
    const repeater_softap_auth_option_t *option =
        find_softap_auth_option_by_mode(s_softap_config.auth_mode);
    return option != NULL ? option->token : "";
}

const repeater_softap_auth_option_t *repeater_settings_get_softap_auth_options(void)
{
    return s_softap_auth_options;
}

size_t repeater_settings_get_softap_auth_option_count(void)
{
    return sizeof(s_softap_auth_options) / sizeof(s_softap_auth_options[0]);
}

bool repeater_settings_is_auto_reboot_enabled(void)
{
    return s_auto_reboot_config.enabled;
}

repeater_auto_reboot_config_t repeater_settings_get_auto_reboot_config(void)
{
    return s_auto_reboot_config;
}

size_t repeater_settings_get_client_history(repeater_client_history_entry_t *entries, size_t max_entries)
{
    size_t count = s_client_history_count;

    if (entries == NULL || max_entries == 0) {
        return count;
    }

    if (count > max_entries) {
        count = max_entries;
    }

    memcpy(entries, s_client_history, count * sizeof(*entries));
    return count;
}

const char *repeater_settings_get_device_description(const char *mac)
{
    char normalized_mac[REPEATER_MAC_STRING_LEN];
    int index;

    if (!normalize_mac_address(mac, normalized_mac, sizeof(normalized_mac))) {
        return "";
    }

    index = find_client_history_index(normalized_mac);
    if (index < 0) {
        return "";
    }

    return s_client_history[index].description;
}

const repeater_station_config_t *repeater_settings_get_station_config(void)
{
    return &s_station_config;
}

const repeater_station_config_t *repeater_settings_get_backup_station_config(void)
{
    return &s_backup_station_config;
}

const repeater_softap_config_t *repeater_settings_get_softap_config(void)
{
    return &s_softap_config;
}

const repeater_web_auth_config_t *repeater_settings_get_web_auth_config(void)
{
    return &s_web_auth_config;
}
