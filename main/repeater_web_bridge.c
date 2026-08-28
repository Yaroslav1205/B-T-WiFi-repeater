#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "repeater_firmware.h"
#include "esp_log.h"
#include "repeater_settings.h"
#include "repeater_web_bridge.h"
#include "repeater_wifi.h"
#include "router_web.h"

#define WIFI_APPLY_DELAY_MS 1000

static const char *TAG = "RepeaterWebBridge";
static repeater_runtime_t *s_runtime;
static TaskHandle_t s_wifi_apply_task_handle;
static repeater_station_config_t s_pending_old_station_config;
static repeater_station_config_t s_pending_old_backup_station_config;
static repeater_softap_config_t s_pending_old_softap_config;
static char s_pending_old_softap_auth_token[16];

static const repeater_station_config_t *bridge_get_station_config(void)
{
    return repeater_settings_get_station_config();
}

static const repeater_station_config_t *bridge_get_backup_station_config(void)
{
    return repeater_settings_get_backup_station_config();
}

static const repeater_softap_config_t *bridge_get_softap_config(void)
{
    return repeater_settings_get_softap_config();
}

static const repeater_web_auth_config_t *bridge_get_web_auth_config(void)
{
    return repeater_settings_get_web_auth_config();
}

static float bridge_get_chip_temperature_c(void)
{
    float celsius = NAN;

    if (s_runtime == NULL || s_runtime->temp_sensor == NULL) {
        return NAN;
    }

    if (temperature_sensor_get_celsius(s_runtime->temp_sensor, &celsius) != ESP_OK) {
        return NAN;
    }

    return celsius;
}

static bool bridge_is_upstream_connected(void)
{
    return repeater_wifi_is_upstream_connected(s_runtime);
}

static bool bridge_is_using_backup_upstream(void)
{
    return repeater_wifi_is_using_backup_upstream(s_runtime);
}

static const char *bridge_get_active_upstream_ssid(void)
{
    return repeater_wifi_get_active_upstream_ssid(s_runtime);
}

static esp_err_t bridge_get_upstream_ip_info(esp_netif_ip_info_t *out_ip_info)
{
    return repeater_wifi_get_upstream_ip_info(s_runtime, out_ip_info);
}

static int64_t bridge_get_upstream_connected_at_epoch(void)
{
    return repeater_wifi_get_upstream_connected_at_epoch(s_runtime);
}

static esp_err_t bridge_get_upstream_packet_counts(uint32_t *out_rx_packets,
                                                   uint32_t *out_tx_packets)
{
    return repeater_wifi_get_upstream_packet_counts(s_runtime, out_rx_packets, out_tx_packets);
}

static int bridge_get_client_count(void)
{
    return repeater_wifi_get_client_count(s_runtime);
}

static size_t bridge_get_clients(repeater_client_info_t *clients, size_t max_clients)
{
    return repeater_wifi_get_clients(clients, max_clients);
}

static void bridge_get_firmware_status(repeater_firmware_status_t *out_status)
{
    repeater_firmware_get_status(out_status);
}

static esp_err_t bridge_check_firmware_update(repeater_firmware_status_t *out_status)
{
    return repeater_firmware_check_now(out_status);
}

static esp_err_t bridge_start_firmware_update(void)
{
    return repeater_firmware_start_update();
}

static esp_err_t bridge_restore_wifi_networks(const repeater_station_config_t *old_station_config,
                                              const repeater_station_config_t *old_backup_station_config,
                                              const repeater_softap_config_t *old_softap_config,
                                              const char *old_softap_auth_token)
{
    ESP_RETURN_ON_ERROR(repeater_settings_set_wifi_networks(old_station_config->ssid,
                                                            old_station_config->password,
                                                            old_backup_station_config->ssid,
                                                            old_backup_station_config->password,
                                                            old_softap_config->ssid,
                                                            old_softap_config->password,
                                                            old_softap_auth_token),
                        TAG, "Failed to restore previous Wi-Fi settings in NVS");

    if (s_runtime != NULL && s_runtime->started) {
        ESP_RETURN_ON_ERROR(repeater_wifi_apply_saved_network_settings(s_runtime, true), TAG,
                            "Failed to restore previous Wi-Fi runtime configuration");
    }

    return ESP_OK;
}

static void bridge_apply_saved_wifi_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(WIFI_APPLY_DELAY_MS));

    if (s_runtime != NULL && s_runtime->started) {
        esp_err_t err = repeater_wifi_apply_saved_network_settings(s_runtime, true);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to apply deferred Wi-Fi settings, restoring previous configuration");
            esp_err_t rollback_err = bridge_restore_wifi_networks(&s_pending_old_station_config,
                                                                  &s_pending_old_backup_station_config,
                                                                  &s_pending_old_softap_config,
                                                                  s_pending_old_softap_auth_token);
            if (rollback_err != ESP_OK) {
                ESP_LOGE(TAG, "Rollback of deferred Wi-Fi settings failed: %s",
                         esp_err_to_name(rollback_err));
            }
        } else {
            ESP_LOGI(TAG, "Applied deferred Station and SoftAP settings from web UI");
        }
    }

    s_wifi_apply_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t bridge_schedule_wifi_apply(void)
{
    if (s_runtime == NULL || !s_runtime->started) {
        return ESP_OK;
    }

    if (s_wifi_apply_task_handle != NULL) {
        ESP_LOGI(TAG, "Wi-Fi apply already pending, keeping latest saved settings");
        return ESP_OK;
    }

    BaseType_t task_result = xTaskCreate(bridge_apply_saved_wifi_task, "wifi_apply", 4096,
                                         NULL, 1, &s_wifi_apply_task_handle);

    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_FAIL, TAG,
                        "Failed to create deferred Wi-Fi apply task");
    return ESP_OK;
}

static esp_err_t bridge_set_wifi_networks(const char *station_ssid,
                                          const char *station_password,
                                          const char *backup_station_ssid,
                                          const char *backup_station_password,
                                          const char *softap_ssid,
                                          const char *softap_password,
                                          const char *softap_auth_token)
{
    const repeater_station_config_t old_station_config = *repeater_settings_get_station_config();
    const repeater_station_config_t old_backup_station_config =
        *repeater_settings_get_backup_station_config();
    const repeater_softap_config_t old_softap_config = *repeater_settings_get_softap_config();
    char old_softap_auth_token[16];

    snprintf(old_softap_auth_token, sizeof(old_softap_auth_token), "%s",
             repeater_settings_get_softap_auth_token());

    ESP_RETURN_ON_ERROR(repeater_settings_set_wifi_networks(station_ssid, station_password,
                                                            backup_station_ssid,
                                                            backup_station_password,
                                                            softap_ssid, softap_password,
                                                            softap_auth_token),
                        TAG, "Failed to save Wi-Fi network settings");

    if (s_runtime == NULL || !s_runtime->started) {
        ESP_LOGI(TAG, "Saved Wi-Fi settings. Runtime apply will happen on next start");
        return ESP_OK;
    }

    if (s_wifi_apply_task_handle == NULL) {
        s_pending_old_station_config = old_station_config;
        s_pending_old_backup_station_config = old_backup_station_config;
        s_pending_old_softap_config = old_softap_config;
        snprintf(s_pending_old_softap_auth_token, sizeof(s_pending_old_softap_auth_token), "%s",
                 old_softap_auth_token);
    }

    ESP_RETURN_ON_ERROR(bridge_schedule_wifi_apply(), TAG,
                        "Failed to schedule deferred Wi-Fi apply");
    ESP_LOGI(TAG, "Saved Wi-Fi settings and scheduled deferred runtime apply");
    return ESP_OK;
}

esp_err_t repeater_web_bridge_start(repeater_runtime_t *runtime)
{
    router_web_context_t web_context = {
        .get_station_config = bridge_get_station_config,
        .get_backup_station_config = bridge_get_backup_station_config,
        .get_softap_config = bridge_get_softap_config,
        .get_web_auth_config = bridge_get_web_auth_config,
        .get_chip_temperature_c = bridge_get_chip_temperature_c,
        .esp_mac = NULL,
        .is_upstream_connected = bridge_is_upstream_connected,
        .is_using_backup_upstream = bridge_is_using_backup_upstream,
        .get_active_upstream_ssid = bridge_get_active_upstream_ssid,
        .get_upstream_ip_info = bridge_get_upstream_ip_info,
        .get_upstream_connected_at_epoch = bridge_get_upstream_connected_at_epoch,
        .get_upstream_packet_counts = bridge_get_upstream_packet_counts,
        .get_client_count = bridge_get_client_count,
        .get_clients = bridge_get_clients,
        .get_signal_level_label = repeater_settings_get_signal_level_label,
        .get_signal_level_token = repeater_settings_get_signal_level_token,
        .set_signal_level_by_token = repeater_settings_set_signal_level_by_token,
        .get_theme_label = repeater_settings_get_theme_label,
        .get_theme_token = repeater_settings_get_theme_token,
        .set_theme_by_token = repeater_settings_set_theme_by_token,
        .get_softap_auth_token = repeater_settings_get_softap_auth_token,
        .get_auto_reboot_config = repeater_settings_get_auto_reboot_config,
        .set_auto_reboot_config = repeater_settings_set_auto_reboot_config,
        .set_device_description = repeater_settings_set_device_description,
        .set_wifi_networks = bridge_set_wifi_networks,
        .set_web_auth = repeater_settings_set_web_auth,
        .get_firmware_status = bridge_get_firmware_status,
        .check_firmware_update = bridge_check_firmware_update,
        .start_firmware_update = bridge_start_firmware_update,
        .signal_levels = repeater_settings_get_signal_levels(),
        .signal_level_count = repeater_settings_get_signal_level_count(),
        .theme_options = repeater_settings_get_theme_options(),
        .theme_option_count = repeater_settings_get_theme_option_count(),
        .softap_auth_options = repeater_settings_get_softap_auth_options(),
        .softap_auth_option_count = repeater_settings_get_softap_auth_option_count(),
    };

    ESP_RETURN_ON_FALSE(runtime != NULL, ESP_ERR_INVALID_ARG, TAG, "Runtime is required");
    s_runtime = runtime;
    ESP_RETURN_ON_ERROR(repeater_wifi_read_softap_mac(runtime->softap_mac, sizeof(runtime->softap_mac)),
                        TAG, "Failed to prepare SoftAP MAC for web UI");

    web_context.esp_mac = runtime->softap_mac;

    return router_web_start(&web_context, &runtime->http_server);
}

