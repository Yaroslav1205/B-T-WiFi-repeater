#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local/esp_wifi_types_native.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#if IP_NAPT
#include "lwip/lwip_napt.h"
#endif
#include "lwip/sys.h"
#include "project_wifi_config.h"
#include "repeater_settings.h"
#include "repeater_wifi.h"

#define DHCPS_OFFER_DNS 0x02

static const char *TAG = "RepeaterWiFi";
static const char *TAG_AP = "RepeaterAP";
static const char *TAG_STA = "RepeaterSTA";
static repeater_runtime_t *s_runtime;
static esp_timer_handle_t s_sta_reconnect_timer;

static bool repeater_wifi_is_using_backup_profile(void);
static const char *repeater_wifi_get_station_role_name(bool use_backup);
static const char *repeater_wifi_get_upstream_ssid(void);

static bool repeater_wifi_is_system_time_valid(time_t now)
{
    struct tm time_info;

    if (now <= 0 || localtime_r(&now, &time_info) == NULL) {
        return false;
    }

    return time_info.tm_year >= (2024 - 1900);
}

static int64_t repeater_wifi_read_current_epoch_if_valid(void)
{
    const time_t now = time(NULL);

    return repeater_wifi_is_system_time_valid(now) ? (int64_t)now : 0;
}

static void repeater_wifi_mark_upstream_connected(void)
{
    if (s_runtime == NULL) {
        return;
    }

    s_runtime->upstream_connected_monotonic_us = esp_timer_get_time();
    s_runtime->upstream_connected_at_epoch = repeater_wifi_read_current_epoch_if_valid();
}

static uint32_t repeater_wifi_get_reconnect_delay_ms(bool initial_attempt, int retry_count)
{
    if (initial_attempt) {
        return PROJECT_WIFI_STA_INITIAL_CONNECT_DELAY_MS;
    }

    const int retry_index = retry_count > 0 ? retry_count - 1 : 0;
    const uint32_t delay_ms = PROJECT_WIFI_STA_RETRY_DELAY_MS +
        (uint32_t)retry_index * PROJECT_WIFI_STA_RETRY_BACKOFF_STEP_MS;

    return delay_ms > PROJECT_WIFI_STA_RETRY_DELAY_MAX_MS
        ? PROJECT_WIFI_STA_RETRY_DELAY_MAX_MS
        : delay_ms;
}

static void repeater_wifi_cancel_scheduled_reconnect(void)
{
    if (s_sta_reconnect_timer == NULL) {
        return;
    }

    esp_err_t err = esp_timer_stop(s_sta_reconnect_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG_STA, "Failed to stop reconnect timer: %s", esp_err_to_name(err));
    }
}

static void repeater_wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;

    if (s_runtime == NULL) {
        return;
    }

    ESP_LOGI(TAG_STA, "Attempting upstream connection to %s SSID: %s",
             repeater_wifi_get_station_role_name(repeater_wifi_is_using_backup_profile()),
             repeater_wifi_get_upstream_ssid());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
}

static esp_err_t repeater_wifi_ensure_reconnect_timer(void)
{
    if (s_sta_reconnect_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = repeater_wifi_reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sta_reconnect",
        .skip_unhandled_events = true,
    };

    return esp_timer_create(&timer_args, &s_sta_reconnect_timer);
}

static void repeater_wifi_schedule_reconnect(bool initial_attempt)
{
    const uint32_t delay_ms = repeater_wifi_get_reconnect_delay_ms(initial_attempt,
                                                                   s_runtime != NULL ? s_runtime->retry_count : 0);

    if (repeater_wifi_ensure_reconnect_timer() != ESP_OK) {
        ESP_LOGW(TAG_STA, "Reconnect timer unavailable, connecting immediately");
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        return;
    }

    repeater_wifi_cancel_scheduled_reconnect();
    ESP_LOGI(TAG_STA, "%s upstream connect scheduled in %lu ms for %s SSID: %s",
             initial_attempt ? "Initial" : "Retry",
             (unsigned long)delay_ms,
             repeater_wifi_get_station_role_name(repeater_wifi_is_using_backup_profile()),
             repeater_wifi_get_upstream_ssid());

    if (delay_ms == 0) {
        repeater_wifi_reconnect_timer_cb(NULL);
        return;
    }

    esp_err_t err = esp_timer_start_once(s_sta_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_STA, "Failed to start reconnect timer: %s. Connecting immediately",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    }
}

static const repeater_station_config_t *repeater_wifi_get_primary_station_config(void)
{
    return repeater_settings_get_station_config();
}

static bool repeater_wifi_station_configs_match(const repeater_station_config_t *left,
                                                const repeater_station_config_t *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    return strcmp(left->ssid, right->ssid) == 0 &&
        strcmp(left->password, right->password) == 0;
}

static bool repeater_wifi_backup_matches_primary(void)
{
    const repeater_station_config_t *primary_station_config = repeater_wifi_get_primary_station_config();
    const repeater_station_config_t *backup_station_config =
        repeater_settings_get_backup_station_config();

    return backup_station_config != NULL && backup_station_config->ssid[0] != '\0' &&
        repeater_wifi_station_configs_match(primary_station_config, backup_station_config);
}

static bool repeater_wifi_has_backup_station(void)
{
    if (!PROJECT_WIFI_STA_BACKUP_ENABLED) {
        return false;
    }

    const repeater_station_config_t *backup_station_config =
        repeater_settings_get_backup_station_config();

    if (backup_station_config == NULL || backup_station_config->ssid[0] == '\0') {
        return false;
    }

    return !repeater_wifi_backup_matches_primary();
}

static const char *repeater_wifi_get_station_role_name(bool use_backup)
{
    return use_backup ? "backup" : "primary";
}

static int repeater_wifi_get_failover_retry_threshold(void)
{
    if (PROJECT_WIFI_STA_FAILOVER_RETRY_THRESHOLD < 1) {
        return 1;
    }

    if (PROJECT_WIFI_STA_FAILOVER_RETRY_THRESHOLD > PROJECT_WIFI_STA_MAXIMUM_RETRY) {
        return PROJECT_WIFI_STA_MAXIMUM_RETRY;
    }

    return PROJECT_WIFI_STA_FAILOVER_RETRY_THRESHOLD;
}

static int repeater_wifi_get_probe_failure_threshold(void)
{
    return PROJECT_UPSTREAM_PROBE_FAILURE_THRESHOLD < 1
        ? 1
        : PROJECT_UPSTREAM_PROBE_FAILURE_THRESHOLD;
}

static int64_t repeater_wifi_read_monotonic_us(void)
{
    return esp_timer_get_time();
}

static void repeater_wifi_begin_timeout_window(int64_t *started_at_monotonic_us)
{
    if (started_at_monotonic_us == NULL || *started_at_monotonic_us > 0) {
        return;
    }

    *started_at_monotonic_us = repeater_wifi_read_monotonic_us();
}

static void repeater_wifi_clear_timeout_window(int64_t *started_at_monotonic_us)
{
    if (started_at_monotonic_us == NULL) {
        return;
    }

    *started_at_monotonic_us = 0;
}

static int64_t repeater_wifi_elapsed_ms_since(int64_t started_at_monotonic_us, int64_t now_monotonic_us)
{
    if (started_at_monotonic_us <= 0 || now_monotonic_us <= started_at_monotonic_us) {
        return 0;
    }

    return (now_monotonic_us - started_at_monotonic_us) / 1000LL;
}

static bool repeater_wifi_timeout_expired(int64_t started_at_monotonic_us, int64_t timeout_ms,
                                          int64_t now_monotonic_us)
{
    return started_at_monotonic_us > 0 &&
        repeater_wifi_elapsed_ms_since(started_at_monotonic_us, now_monotonic_us) >= timeout_ms;
}

static void repeater_wifi_restart_like_power_cycle(const char *reason)
{
    ESP_LOGE(TAG, "Failsafe reboot requested: %s", reason != NULL ? reason : "unknown reason");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static int repeater_wifi_refresh_active_client_count(void)
{
    wifi_sta_list_t sta_list = { 0 };

    if (s_runtime == NULL) {
        return 0;
    }

    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        s_runtime->ap_client_count = sta_list.num;
    }

    return s_runtime->ap_client_count;
}

static const repeater_station_config_t *repeater_wifi_get_station_config_for_profile(bool use_backup)
{
    if (use_backup && repeater_wifi_has_backup_station()) {
        return repeater_settings_get_backup_station_config();
    }

    return repeater_wifi_get_primary_station_config();
}

static bool repeater_wifi_is_using_backup_profile(void)
{
    return s_runtime != NULL && s_runtime->sta_using_backup_connection && repeater_wifi_has_backup_station();
}

static const repeater_station_config_t *repeater_wifi_get_station_config(void)
{
    return repeater_wifi_get_station_config_for_profile(repeater_wifi_is_using_backup_profile());
}

static const repeater_softap_config_t *repeater_wifi_get_softap_config(void)
{
    return repeater_settings_get_softap_config();
}

static const char *repeater_wifi_get_upstream_ssid(void)
{
    return repeater_wifi_get_station_config()->ssid;
}

static void repeater_wifi_reset_upstream_state(void)
{
    if (s_runtime == NULL) {
        return;
    }

    s_runtime->retry_count = 0;
    s_runtime->upstream_probe_failure_count = 0;
    s_runtime->sta_has_ip_address = false;
    s_runtime->sta_has_upstream_connection = false;
}

static void repeater_wifi_failsafe_reboot_task(void *arg)
{
    (void)arg;

    while (true) {
        const int64_t now_monotonic_us = repeater_wifi_read_monotonic_us();

        if (PROJECT_FAILSAFE_REBOOT_ENABLED && s_runtime != NULL) {
            const int active_client_count = repeater_wifi_refresh_active_client_count();

            if (active_client_count > 0) {
                repeater_wifi_clear_timeout_window(&s_runtime->no_client_since_monotonic_us);
            } else {
                repeater_wifi_begin_timeout_window(&s_runtime->no_client_since_monotonic_us);
            }

            if (repeater_wifi_timeout_expired(s_runtime->upstream_unavailable_since_monotonic_us,
                                              PROJECT_FAILSAFE_REBOOT_TIMEOUT_MS,
                                              now_monotonic_us)) {
                const int64_t elapsed_ms = repeater_wifi_elapsed_ms_since(
                    s_runtime->upstream_unavailable_since_monotonic_us, now_monotonic_us);

                ESP_LOGE(TAG_STA,
                         "No upstream IP for %lld ms on %s SSID=%s, forcing reboot",
                         elapsed_ms,
                         repeater_wifi_get_station_role_name(repeater_wifi_is_using_backup_profile()),
                         repeater_wifi_get_upstream_ssid());
                repeater_wifi_restart_like_power_cycle("upstream unavailable for too long");
            }

            if (repeater_wifi_timeout_expired(s_runtime->no_client_since_monotonic_us,
                                              PROJECT_FAILSAFE_REBOOT_TIMEOUT_MS,
                                              now_monotonic_us)) {
                const int64_t elapsed_ms = repeater_wifi_elapsed_ms_since(
                    s_runtime->no_client_since_monotonic_us, now_monotonic_us);

                ESP_LOGW(TAG_AP,
                         "No SoftAP clients connected for %lld ms, forcing reboot",
                         elapsed_ms);
                repeater_wifi_restart_like_power_cycle("no SoftAP clients for too long");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PROJECT_FAILSAFE_REBOOT_CHECK_INTERVAL_MS));
    }
}

static const char *wifi_disconnect_reason_to_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth expired";
    case WIFI_REASON_AUTH_LEAVE:
        return "auth leave";
    case WIFI_REASON_AUTH_FAIL:
        return "auth failed";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "assoc expired";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "assoc too many";
    case WIFI_REASON_NOT_AUTHED:
        return "not authed";
    case WIFI_REASON_NOT_ASSOCED:
        return "not assoced";
    case WIFI_REASON_ASSOC_LEAVE:
        return "assoc leave";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "assoc not authed";
    case WIFI_REASON_NO_AP_FOUND:
        return "AP not found";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4-way handshake timeout";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "group key update timeout";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "RSN IE differs in 4-way handshake";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "group cipher invalid";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "pairwise cipher invalid";
    case WIFI_REASON_AKMP_INVALID:
        return "AKMP invalid";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "unsupported RSN IE version";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "invalid RSN IE capabilities";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802.1X auth failed";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "cipher suite rejected";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "handshake timeout";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "beacon timeout";
    case WIFI_REASON_ASSOC_FAIL:
        return "association failed";
#ifdef WIFI_REASON_CONNECTION_FAIL
    case WIFI_REASON_CONNECTION_FAIL:
        return "connection failed";
#endif
#ifdef WIFI_REASON_AP_TSF_RESET
    case WIFI_REASON_AP_TSF_RESET:
        return "AP TSF reset";
#endif
    default:
        return "other";
    }
}

static const char *wifi_disconnect_reason_detail(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
        return "the access point did not complete authentication in time";
    case WIFI_REASON_AUTH_LEAVE:
        return "the access point or client left during authentication";
    case WIFI_REASON_AUTH_FAIL:
        return "authentication was rejected, often due to a wrong password or unsupported security mode";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "association did not complete before timeout";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "the access point rejected the client because too many stations are already connected";
    case WIFI_REASON_NOT_AUTHED:
        return "the station tried to continue before successful authentication";
    case WIFI_REASON_NOT_ASSOCED:
        return "the station is not associated with the access point";
    case WIFI_REASON_ASSOC_LEAVE:
        return "the association ended while joining the access point";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "association was rejected because authentication was not accepted";
    case WIFI_REASON_NO_AP_FOUND:
        return "the configured SSID was not found on the air";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "the WPA 4-way handshake did not complete, often due to a password or WPA compatibility issue";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "group key negotiation timed out after association";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "the RSN security parameters changed during WPA negotiation";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "the access point rejected the configured group cipher";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "the access point rejected the configured pairwise cipher";
    case WIFI_REASON_AKMP_INVALID:
        return "the authentication or key-management suite is not accepted by the access point";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "the access point reported an unsupported RSN information element version";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "the RSN capabilities reported by the access point are not accepted";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802.1X or enterprise authentication failed";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "the access point rejected the offered cipher suite";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "key handshake timed out, often due to weak signal or WPA mismatch";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "the station stopped hearing beacons from the access point";
    case WIFI_REASON_ASSOC_FAIL:
        return "the access point rejected association after authentication";
#ifdef WIFI_REASON_CONNECTION_FAIL
    case WIFI_REASON_CONNECTION_FAIL:
        return "the driver could not finish the connection sequence";
#endif
#ifdef WIFI_REASON_AP_TSF_RESET
    case WIFI_REASON_AP_TSF_RESET:
        return "the access point timing changed unexpectedly during connection";
#endif
    default:
        return "the Wi-Fi stack reported an unclassified disconnect reason";
    }
}

static const char *wifi_auth_mode_to_str(wifi_auth_mode_t auth_mode)
{
    switch (auth_mode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3-PSK";
    default:
        return "unknown";
    }
}

static const char *repeater_wifi_get_softap_channel_label(void)
{
    return PROJECT_WIFI_AP_CHANNEL == 0 ? "auto" : "fixed";
}

static void repeater_wifi_format_mac_uppercase(char *output, size_t output_size,
                                               const uint8_t mac[6])
{
    if (output == NULL || mac == NULL || output_size < REPEATER_MAC_STRING_LEN) {
        return;
    }

    snprintf(output, output_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool repeater_probe_internet_once(void)
{
    const esp_http_client_config_t config = {
        .url = PROJECT_UPSTREAM_PROBE_URL,
        .timeout_ms = PROJECT_UPSTREAM_PROBE_TIMEOUT_MS,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        ESP_LOGE(TAG_STA, "Failed to create upstream probe client");
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    bool has_internet = false;

    if (err == ESP_OK) {
        const int status_code = esp_http_client_get_status_code(client);
        has_internet = status_code >= 200 && status_code < 400;

        if (!has_internet) {
            ESP_LOGW(TAG_STA, "Upstream probe returned HTTP %d", status_code);
        }
    } else {
        ESP_LOGW(TAG_STA, "Upstream probe failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return has_internet;
}

static void repeater_update_internet_status(bool has_internet)
{
    const bool previous = s_runtime->sta_has_upstream_connection;

    s_runtime->sta_has_upstream_connection = has_internet;

    if (has_internet && !previous) {
        ESP_LOGI(TAG_STA, "Upstream internet access confirmed");
    } else if (!has_internet && previous) {
        ESP_LOGW(TAG_STA, "Upstream internet access lost");
    }
}

static esp_err_t repeater_apply_station_config_for_profile(bool use_backup, bool reconnect_now)
{
    wifi_config_t wifi_sta_config = { 0 };
    const repeater_station_config_t *station_config =
        repeater_wifi_get_station_config_for_profile(use_backup);

    memcpy(wifi_sta_config.sta.ssid, station_config->ssid, strlen(station_config->ssid));
    snprintf((char *)wifi_sta_config.sta.password, sizeof(wifi_sta_config.sta.password), "%s",
             station_config->password);
    wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_sta_config.sta.failure_retry_cnt = PROJECT_WIFI_STA_MAXIMUM_RETRY;
    wifi_sta_config.sta.threshold.authmode = station_config->password[0] == '\0'
        ? WIFI_AUTH_OPEN
        : PROJECT_WIFI_STA_SCAN_AUTH_MODE;
    wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config), TAG_STA,
                        "Failed to configure upstream station");

    if (s_runtime != NULL) {
        s_runtime->sta_using_backup_connection = use_backup && repeater_wifi_has_backup_station();
    }

    if (reconnect_now) {
        repeater_wifi_reset_upstream_state();
        repeater_wifi_cancel_scheduled_reconnect();
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
        repeater_wifi_schedule_reconnect(false);
    }

    ESP_LOGI(TAG_STA, "Station configured. Role=%s SSID=%s",
             repeater_wifi_get_station_role_name(use_backup), station_config->ssid);
    return ESP_OK;
}

static esp_err_t repeater_switch_upstream_profile(bool use_backup, const char *reason, bool reconnect_now)
{
    if (use_backup && !repeater_wifi_has_backup_station()) {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_runtime != NULL && repeater_wifi_is_using_backup_profile() == use_backup) {
        return repeater_apply_station_config_for_profile(use_backup, reconnect_now);
    }

    ESP_LOGW(TAG_STA, "Switching upstream to %s SSID=%s (%s)",
             repeater_wifi_get_station_role_name(use_backup),
             repeater_wifi_get_station_config_for_profile(use_backup)->ssid,
             reason != NULL ? reason : "no reason");

    repeater_wifi_reset_upstream_state();
    return repeater_apply_station_config_for_profile(use_backup, reconnect_now);
}

static bool repeater_switch_to_alternate_upstream(const char *reason, bool reconnect_now)
{
    const bool next_use_backup = !repeater_wifi_is_using_backup_profile();
    esp_err_t err = repeater_switch_upstream_profile(next_use_backup, reason, reconnect_now);

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG_STA, "Backup upstream is not configured, staying on primary");
        return false;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG_STA, "Failed to switch upstream profile: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static void repeater_internet_probe_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_runtime == NULL || !s_runtime->sta_has_ip_address) {
            repeater_update_internet_status(false);
            vTaskDelay(pdMS_TO_TICKS(PROJECT_UPSTREAM_PROBE_RETRY_MS));
            continue;
        }

        const bool has_internet = repeater_probe_internet_once();
        repeater_update_internet_status(has_internet);

        if (has_internet) {
            s_runtime->upstream_probe_failure_count = 0;
        } else {
            ++s_runtime->upstream_probe_failure_count;
            ESP_LOGW(TAG_STA, "Upstream probe failed %d/%d on %s SSID=%s",
                     s_runtime->upstream_probe_failure_count,
                     repeater_wifi_get_probe_failure_threshold(),
                     repeater_wifi_get_station_role_name(repeater_wifi_is_using_backup_profile()),
                     repeater_wifi_get_upstream_ssid());

            if (s_runtime->upstream_probe_failure_count >=
                repeater_wifi_get_probe_failure_threshold()) {
                if (repeater_switch_to_alternate_upstream("internet probe failures", true)) {
                    vTaskDelay(pdMS_TO_TICKS(PROJECT_UPSTREAM_PROBE_RETRY_MS));
                    continue;
                }

                s_runtime->upstream_probe_failure_count = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(has_internet
            ? PROJECT_UPSTREAM_PROBE_INTERVAL_MS
            : PROJECT_UPSTREAM_PROBE_RETRY_MS));
    }
}

size_t repeater_wifi_get_clients(repeater_client_info_t *clients, size_t max_clients)
{
    wifi_sta_list_t sta_list;
    repeater_client_history_entry_t *history = NULL;
    size_t history_count = 0;
    size_t count = 0;

    if (clients == NULL || max_clients == 0) {
        return 0;
    }

    memset(clients, 0, max_clients * sizeof(*clients));
    history = calloc(REPEATER_CLIENT_HISTORY_MAX_ENTRIES, sizeof(*history));
    if (history != NULL) {
        history_count = repeater_settings_get_client_history(history,
                                                             REPEATER_CLIENT_HISTORY_MAX_ENTRIES);
    }

    for (size_t i = 0; i < history_count && count < max_clients; ++i) {
        snprintf(clients[count].mac, sizeof(clients[count].mac), "%s", history[i].mac);
        snprintf(clients[count].description, sizeof(clients[count].description), "%s",
                 history[i].description);
        clients[count].rssi = 0;
        clients[count].is_connected = false;
        clients[count].first_seen_epoch = history[i].first_seen_epoch;
        clients[count].last_seen_epoch = history[i].last_seen_epoch;
        ++count;
    }

    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK) {
        free(history);
        return count;
    }

    for (int i = 0; i < sta_list.num; ++i) {
        char normalized_mac[REPEATER_MAC_STRING_LEN];
        size_t client_index = count;

        repeater_wifi_format_mac_uppercase(normalized_mac, sizeof(normalized_mac), sta_list.sta[i].mac);

        for (size_t j = 0; j < count; ++j) {
            if (strcmp(clients[j].mac, normalized_mac) == 0) {
                client_index = j;
                break;
            }
        }

        if (client_index == count) {
            if (count >= max_clients) {
                continue;
            }

            snprintf(clients[count].mac, sizeof(clients[count].mac), "%s", normalized_mac);
            snprintf(clients[count].description, sizeof(clients[count].description), "%s",
                     repeater_settings_get_device_description(normalized_mac));
            clients[count].first_seen_epoch = 0;
            clients[count].last_seen_epoch = 0;
            ++count;
        }

        clients[client_index].rssi = sta_list.sta[i].rssi;
        clients[client_index].is_connected = true;
    }

    free(history);
    return count;
}

static void repeater_update_softap_dns(void)
{
    esp_netif_dns_info_t dns;
    uint8_t dns_offer = DHCPS_OFFER_DNS;
    esp_err_t err = esp_netif_get_dns_info(s_runtime->sta_netif, ESP_NETIF_DNS_MAIN, &dns);

    if (err != ESP_OK) {
        ESP_LOGW(TAG_AP, "Failed to get DNS from upstream station: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_runtime->ap_netif));

    err = esp_netif_dhcps_option(s_runtime->ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER, &dns_offer, sizeof(dns_offer));
    if (err != ESP_OK) {
        ESP_LOGE(TAG_AP, "Failed to enable DNS offer on SoftAP DHCP: %s", esp_err_to_name(err));
        return;
    }

    err = esp_netif_set_dns_info(s_runtime->ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_AP, "Failed to copy upstream DNS to SoftAP DHCP: %s", esp_err_to_name(err));
        return;
    }

    err = esp_netif_dhcps_start(s_runtime->ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_AP, "Failed to restart SoftAP DHCP server: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG_AP, "SoftAP DHCP DNS updated from upstream station");
}

static void repeater_enable_napt(void)
{
#if IP_NAPT
    esp_err_t err;
    esp_netif_ip_info_t ap_ip_info;

    if (s_runtime->napt_enabled) {
        return;
    }

    err = esp_netif_napt_enable(s_runtime->ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_AP, "Failed to enable NAPT: %s", esp_err_to_name(err));
        return;
    }

    s_runtime->napt_enabled = true;

    if (esp_netif_get_ip_info(s_runtime->ap_netif, &ap_ip_info) == ESP_OK) {
        ESP_LOGI(TAG_AP, "NAPT enabled on SoftAP IP: " IPSTR, IP2STR(&ap_ip_info.ip));
    } else {
        ESP_LOGI(TAG_AP, "NAPT enabled on SoftAP");
    }
#endif
}

static void repeater_handle_upstream_ready(const ip_event_got_ip_t *event)
{
    esp_err_t err = esp_netif_set_default_netif(s_runtime->sta_netif);

    if (err != ESP_OK) {
        ESP_LOGW(TAG_STA, "Failed to set upstream station as default netif: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG_STA, "Gateway:" IPSTR " Netmask:" IPSTR,
             IP2STR(&event->ip_info.gw), IP2STR(&event->ip_info.netmask));

    repeater_update_softap_dns();
    repeater_enable_napt();
}

static void repeater_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                        int32_t event_id, void *event_data)
{
    (void)arg;

    if (s_runtime == NULL) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = (const wifi_event_ap_staconnected_t *)event_data;
        char mac_text[REPEATER_MAC_STRING_LEN];

        repeater_wifi_format_mac_uppercase(mac_text, sizeof(mac_text), event->mac);
        ESP_ERROR_CHECK_WITHOUT_ABORT(repeater_settings_record_client_connection(mac_text));
        ++s_runtime->ap_client_count;
        repeater_wifi_clear_timeout_window(&s_runtime->no_client_since_monotonic_us);
        ESP_LOGI(TAG_AP, "Station %s joined, AID=%d", mac_text, event->aid);
        ESP_LOGI(TAG_AP, "Connected SoftAP clients: %d", repeater_wifi_refresh_active_client_count());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = (const wifi_event_ap_stadisconnected_t *)event_data;
        char mac_text[REPEATER_MAC_STRING_LEN];

        repeater_wifi_format_mac_uppercase(mac_text, sizeof(mac_text), event->mac);
        if (s_runtime->ap_client_count > 0) {
            --s_runtime->ap_client_count;
        }
        if (s_runtime->ap_client_count == 0) {
            repeater_wifi_begin_timeout_window(&s_runtime->no_client_since_monotonic_us);
        }

        ESP_LOGI(TAG_AP, "Station %s left, AID=%d, reason=%d",
                 mac_text, event->aid, event->reason);
        ESP_LOGI(TAG_AP, "Connected SoftAP clients: %d", repeater_wifi_refresh_active_client_count());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        repeater_wifi_schedule_reconnect(true);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        repeater_wifi_cancel_scheduled_reconnect();
        repeater_wifi_mark_upstream_connected();
        ESP_LOGI(TAG_STA, "Associated with %s upstream SSID: %s",
                 repeater_wifi_get_station_role_name(repeater_wifi_is_using_backup_profile()),
                 repeater_wifi_get_upstream_ssid());
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        const int failover_threshold = repeater_wifi_get_failover_retry_threshold();

        s_runtime->sta_has_ip_address = false;
        s_runtime->sta_has_upstream_connection = false;
        s_runtime->upstream_probe_failure_count = 0;
        repeater_wifi_begin_timeout_window(&s_runtime->upstream_unavailable_since_monotonic_us);

        if (s_runtime->retry_count < PROJECT_WIFI_STA_MAXIMUM_RETRY) {
            ++s_runtime->retry_count;
            ESP_LOGW(TAG_STA, "Upstream disconnect, retry %d/%d, reason=%u (%s): %s",
                     s_runtime->retry_count, PROJECT_WIFI_STA_MAXIMUM_RETRY,
                     event->reason, wifi_disconnect_reason_to_str(event->reason),
                     wifi_disconnect_reason_detail(event->reason));
        } else {
            ESP_LOGW(TAG_STA, "Upstream still unavailable after %d retries, continuing reconnect loop",
                     PROJECT_WIFI_STA_MAXIMUM_RETRY);
            ESP_LOGW(TAG_STA, "Last disconnect reason=%u (%s): %s",
                     event->reason, wifi_disconnect_reason_to_str(event->reason),
                     wifi_disconnect_reason_detail(event->reason));
        }

        if (s_runtime->retry_count >= failover_threshold) {
            (void)repeater_switch_to_alternate_upstream("disconnect retry threshold reached", false);
        }

        repeater_wifi_schedule_reconnect(false);

        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        repeater_wifi_cancel_scheduled_reconnect();
        s_runtime->sta_has_ip_address = true;
        s_runtime->sta_has_upstream_connection = false;
        s_runtime->retry_count = 0;
        s_runtime->upstream_probe_failure_count = 0;
        repeater_wifi_clear_timeout_window(&s_runtime->upstream_unavailable_since_monotonic_us);
        repeater_handle_upstream_ready(event);
        ESP_LOGI(TAG_STA, "Checking upstream internet access via %s", PROJECT_UPSTREAM_PROBE_URL);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        const ip_event_ap_staipassigned_t *event = (const ip_event_ap_staipassigned_t *)event_data;
        char mac_text[REPEATER_MAC_STRING_LEN];

        repeater_wifi_format_mac_uppercase(mac_text, sizeof(mac_text), event->mac);
        ESP_LOGI(TAG_AP, "Assigned IP to SoftAP client: " IPSTR ", MAC=%s",
                 IP2STR(&event->ip), mac_text);
    }
}

static esp_err_t repeater_register_event_handlers(void)
{
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &repeater_wifi_event_handler,
                                                            NULL, NULL),
                        TAG, "Failed to register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &repeater_wifi_event_handler,
                                                            NULL, NULL),
                        TAG, "Failed to register STA IP event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                            &repeater_wifi_event_handler,
                                                            NULL, NULL),
                        TAG, "Failed to register SoftAP IP event handler");
    return ESP_OK;
}

static esp_err_t repeater_start_internet_probe(void)
{
    if (s_runtime->internet_probe_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t task_result = xTaskCreate(repeater_internet_probe_task, "internet_probe", 4096,
                                         NULL, 1, &s_runtime->internet_probe_task_handle);

    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_FAIL, TAG_STA,
                        "Failed to create upstream internet probe task");
    return ESP_OK;
}

static esp_err_t repeater_start_failsafe_reboot_monitor(void)
{
    if (!PROJECT_FAILSAFE_REBOOT_ENABLED || s_runtime->failsafe_reboot_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t task_result = xTaskCreate(repeater_wifi_failsafe_reboot_task, "failsafe_reboot",
                                         3072, NULL, 1, &s_runtime->failsafe_reboot_task_handle);

    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_FAIL, TAG,
                        "Failed to create failsafe reboot task");
    return ESP_OK;
}

static esp_err_t repeater_apply_softap_config(void)
{
    wifi_config_t wifi_ap_config = { 0 };
    const repeater_softap_config_t *softap_config = repeater_wifi_get_softap_config();

    wifi_ap_config.ap.ssid_len = strlen(softap_config->ssid);
    memcpy(wifi_ap_config.ap.ssid, softap_config->ssid, wifi_ap_config.ap.ssid_len);
    wifi_ap_config.ap.channel = PROJECT_WIFI_AP_CHANNEL;
    snprintf((char *)wifi_ap_config.ap.password, sizeof(wifi_ap_config.ap.password), "%s",
             softap_config->password);
    wifi_ap_config.ap.max_connection = PROJECT_WIFI_AP_MAX_STA_CONN;
    wifi_ap_config.ap.ssid_hidden = 0;
    wifi_ap_config.ap.beacon_interval = 100;
    wifi_ap_config.ap.authmode = softap_config->auth_mode == WIFI_AUTH_OPEN
        ? WIFI_AUTH_OPEN
        : softap_config->auth_mode;
    wifi_ap_config.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config), TAG_AP,
                        "Failed to configure SoftAP");

    ESP_LOGI(TAG_AP, "SoftAP configured. SSID=%s channel=%d (%s) max_clients=%d",
             softap_config->ssid, PROJECT_WIFI_AP_CHANNEL,
             repeater_wifi_get_softap_channel_label(), PROJECT_WIFI_AP_MAX_STA_CONN);
    ESP_LOGI(TAG_AP, "SoftAP security mode: %s",
             wifi_auth_mode_to_str(wifi_ap_config.ap.authmode));
    return ESP_OK;
}

static esp_err_t repeater_init_softap(void)
{
    s_runtime->ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_runtime->ap_netif != NULL, ESP_FAIL, TAG_AP,
                        "Failed to create SoftAP netif");

    return repeater_apply_softap_config();
}

static esp_err_t repeater_init_station(void)
{
    s_runtime->sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_runtime->sta_netif != NULL, ESP_FAIL, TAG_STA,
                        "Failed to create station netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_runtime->sta_netif, PROJECT_WIFI_STA_HOSTNAME),
                        TAG_STA, "Failed to set station hostname");

    return repeater_apply_station_config_for_profile(false, false);
}

esp_err_t repeater_wifi_start(repeater_runtime_t *runtime)
{
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();

    ESP_RETURN_ON_FALSE(runtime != NULL, ESP_ERR_INVALID_ARG, TAG, "Runtime is required");

    s_runtime = runtime;
    s_runtime->sta_using_backup_connection = false;
    s_runtime->retry_count = 0;
    s_runtime->upstream_probe_failure_count = 0;
    s_runtime->upstream_connected_at_epoch = 0;
    s_runtime->upstream_connected_monotonic_us = 0;
    s_runtime->upstream_unavailable_since_monotonic_us = repeater_wifi_read_monotonic_us();
    s_runtime->no_client_since_monotonic_us = repeater_wifi_read_monotonic_us();
    s_runtime->packet_stats_enabled = false;

    if (PROJECT_WIFI_STA_BACKUP_ENABLED && repeater_wifi_backup_matches_primary()) {
        ESP_LOGW(TAG_STA, "Backup upstream matches primary credentials, automatic failover is disabled");
    }

    ESP_RETURN_ON_ERROR(repeater_register_event_handlers(), TAG,
                        "Failed to register event handlers");
    ESP_RETURN_ON_ERROR(repeater_wifi_ensure_reconnect_timer(), TAG,
                        "Failed to create delayed STA reconnect timer");
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), TAG, "Failed to initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Failed to disable power save");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                        "Failed to set AP+STA mode");
    ESP_RETURN_ON_ERROR(repeater_init_softap(), TAG, "Failed to initialize SoftAP");
    ESP_RETURN_ON_ERROR(repeater_init_station(), TAG, "Failed to initialize station");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20), TAG_AP,
                        "Failed to set SoftAP bandwidth");
    ESP_RETURN_ON_ERROR(repeater_settings_apply(), TAG,
                        "Failed to apply saved repeater settings");
    ESP_RETURN_ON_ERROR(repeater_start_internet_probe(), TAG,
                        "Failed to start upstream internet probe");
    ESP_RETURN_ON_ERROR(repeater_start_failsafe_reboot_monitor(), TAG,
                        "Failed to start failsafe reboot monitor");

    return ESP_OK;
}

esp_err_t repeater_wifi_apply_saved_network_settings(repeater_runtime_t *runtime, bool reconnect_now)
{
    ESP_RETURN_ON_FALSE(runtime != NULL, ESP_ERR_INVALID_ARG, TAG, "Runtime is required");
    ESP_RETURN_ON_FALSE(s_runtime == runtime, ESP_ERR_INVALID_STATE, TAG,
                        "Wi-Fi runtime is not initialized");

    ESP_RETURN_ON_ERROR(repeater_apply_softap_config(), TAG,
                        "Failed to apply updated SoftAP settings");
    ESP_RETURN_ON_ERROR(repeater_switch_upstream_profile(false, "saved settings applied", reconnect_now), TAG,
                        "Failed to apply updated station settings");
    return ESP_OK;
}

esp_err_t repeater_wifi_get_softap_ip_info(const repeater_runtime_t *runtime,
                                           esp_netif_ip_info_t *out_ip_info)
{
    ESP_RETURN_ON_FALSE(runtime != NULL, ESP_ERR_INVALID_ARG, TAG, "Runtime is required");
    ESP_RETURN_ON_FALSE(runtime->ap_netif != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "SoftAP netif is unavailable");
    ESP_RETURN_ON_FALSE(out_ip_info != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Output IP info is required");

    return esp_netif_get_ip_info(runtime->ap_netif, out_ip_info);
}

esp_err_t repeater_wifi_get_upstream_ip_info(const repeater_runtime_t *runtime,
                                             esp_netif_ip_info_t *out_ip_info)
{
    ESP_RETURN_ON_FALSE(runtime != NULL, ESP_ERR_INVALID_ARG, TAG, "Runtime is required");
    ESP_RETURN_ON_FALSE(runtime->sta_netif != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Station netif is unavailable");
    ESP_RETURN_ON_FALSE(out_ip_info != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Output IP info is required");

    return esp_netif_get_ip_info(runtime->sta_netif, out_ip_info);
}

esp_err_t repeater_wifi_read_softap_mac(char *out_mac, size_t out_mac_size)
{
    uint8_t ap_mac[6];

    ESP_RETURN_ON_FALSE(out_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "Output MAC buffer is required");
    ESP_RETURN_ON_FALSE(out_mac_size >= REPEATER_MAC_STRING_LEN, ESP_ERR_INVALID_SIZE, TAG,
                        "Output MAC buffer is too small");
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_AP, ap_mac), TAG,
                        "Failed to read SoftAP MAC address");

    repeater_wifi_format_mac_uppercase(out_mac, out_mac_size, ap_mac);
    return ESP_OK;
}

bool repeater_wifi_is_upstream_connected(const repeater_runtime_t *runtime)
{
    return runtime != NULL && runtime->sta_has_upstream_connection;
}

bool repeater_wifi_is_using_backup_upstream(const repeater_runtime_t *runtime)
{
    return runtime != NULL && runtime->sta_using_backup_connection && repeater_wifi_has_backup_station();
}

const char *repeater_wifi_get_active_upstream_ssid(const repeater_runtime_t *runtime)
{
    if (runtime == NULL) {
        return "";
    }

    return repeater_wifi_get_station_config_for_profile(
        runtime->sta_using_backup_connection && repeater_wifi_has_backup_station())->ssid;
}

int64_t repeater_wifi_get_upstream_connected_at_epoch(const repeater_runtime_t *runtime)
{
    const int64_t now_epoch = repeater_wifi_read_current_epoch_if_valid();
    int64_t connected_at_epoch;
    int64_t elapsed_seconds;

    if (runtime == NULL || runtime->upstream_connected_monotonic_us <= 0) {
        return 0;
    }

    if (runtime->upstream_connected_at_epoch > 0) {
        return runtime->upstream_connected_at_epoch;
    }

    if (now_epoch <= 0) {
        return 0;
    }

    elapsed_seconds = (esp_timer_get_time() - runtime->upstream_connected_monotonic_us) / 1000000LL;
    if (elapsed_seconds < 0 || now_epoch <= elapsed_seconds) {
        return 0;
    }

    connected_at_epoch = now_epoch - elapsed_seconds;
    return connected_at_epoch > 0 ? connected_at_epoch : 0;
}

esp_err_t repeater_wifi_get_upstream_packet_counts(const repeater_runtime_t *runtime,
                                                   uint32_t *out_rx_packets,
                                                   uint32_t *out_tx_packets)
{
    ESP_RETURN_ON_FALSE(runtime != NULL, ESP_ERR_INVALID_ARG, TAG, "Runtime is required");
    ESP_RETURN_ON_FALSE(out_rx_packets != NULL && out_tx_packets != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Packet counter outputs are required");
    ESP_RETURN_ON_FALSE(s_runtime == runtime, ESP_ERR_INVALID_STATE, TAG,
                        "Wi-Fi runtime is not initialized");

    *out_rx_packets = 0;
    *out_tx_packets = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

int repeater_wifi_get_client_count(const repeater_runtime_t *runtime)
{
    if (runtime == NULL) {
        return 0;
    }

    if (runtime == s_runtime) {
        return repeater_wifi_refresh_active_client_count();
    }

    return runtime->ap_client_count;
}





