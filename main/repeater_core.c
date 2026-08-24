#include <stdio.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/temperature_sensor.h"
#include "lwip/inet.h"
#include "project_wifi_config.h"
#include "repeater_core.h"
#include "repeater_firmware.h"
#include "repeater_runtime.h"
#include "repeater_settings.h"
#include "repeater_web_bridge.h"
#include "repeater_wifi.h"

static const char *TAG = "RepeaterCore";
static repeater_runtime_t s_runtime;

static float repeater_core_get_chip_temperature_c(void)
{
    float celsius = NAN;

    if (s_runtime.temp_sensor == NULL) {
        return NAN;
    }

    if (temperature_sensor_get_celsius(s_runtime.temp_sensor, &celsius) != ESP_OK) {
        return NAN;
    }

    return celsius;
}

static void repeater_core_format_temperature_text(float celsius, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    if (isnan(celsius)) {
        snprintf(output, output_size, "%s", "n/a");
        return;
    }

    const int scaled = celsius >= 0.0f
        ? (int)(celsius * 10.0f + 0.5f)
        : (int)(celsius * 10.0f - 0.5f);
    const int whole = scaled / 10;
    const int fraction = abs(scaled % 10);

    snprintf(output, output_size, "%d.%dC", whole, fraction);
}

static void repeater_log_task(void *arg)
{
    (void)arg;

    while (true) {
        const repeater_auto_reboot_config_t auto_reboot_config =
            repeater_settings_get_auto_reboot_config();
        const repeater_softap_config_t *softap_config = repeater_settings_get_softap_config();
        const float chip_temperature_c = repeater_core_get_chip_temperature_c();
        char temperature_text[24];

        repeater_core_format_temperature_text(chip_temperature_c, temperature_text,
                                              sizeof(temperature_text));

        ESP_LOGI(TAG,
                 "Status: internet=%s, upstream=%s(%s), softap=%s, clients=%d, tx=%s, theme=%s, auto_reboot=%s %02u:%02u, temp=%s, web=http://192.168.4.1/",
                 repeater_wifi_is_upstream_connected(&s_runtime) ? "online" : "offline",
                 repeater_wifi_get_active_upstream_ssid(&s_runtime),
                 repeater_wifi_is_using_backup_upstream(&s_runtime) ? "backup" : "primary",
                 softap_config->ssid,
                 repeater_wifi_get_client_count(&s_runtime),
                 repeater_settings_get_signal_level_label(),
                 repeater_settings_get_theme_label(),
                 auto_reboot_config.enabled ? "on" : "off",
                 auto_reboot_config.hour,
                 auto_reboot_config.minute,
                 temperature_text);
        vTaskDelay(pdMS_TO_TICKS(PROJECT_STATUS_LOG_INTERVAL_MS));
    }
}

static esp_err_t repeater_init_temperature_sensor(void)
{
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);

    ESP_RETURN_ON_ERROR(temperature_sensor_install(&temp_sensor_config, &s_runtime.temp_sensor), TAG,
                        "Failed to install temperature sensor");
    ESP_RETURN_ON_ERROR(temperature_sensor_enable(s_runtime.temp_sensor), TAG,
                        "Failed to enable temperature sensor");
    return ESP_OK;
}

static esp_err_t repeater_start_status_logging(void)
{
    if (s_runtime.status_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t task_result = xTaskCreate(repeater_log_task, "repeater_log", 4096, NULL, 1,
                                         &s_runtime.status_task_handle);

    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_FAIL, TAG,
                        "Failed to create status log task");
    return ESP_OK;
}

esp_err_t repeater_core_start(void)
{
    esp_netif_ip_info_t ap_ip_info;

    ESP_RETURN_ON_FALSE(!s_runtime.started, ESP_ERR_INVALID_STATE, TAG,
                        "Repeater core already started");

    memset(&s_runtime, 0, sizeof(s_runtime));

    ESP_RETURN_ON_ERROR(repeater_init_temperature_sensor(), TAG,
                        "Failed to initialize temperature sensor");
    ESP_RETURN_ON_ERROR(repeater_wifi_start(&s_runtime), TAG, "Failed to start Wi-Fi runtime");
    ESP_RETURN_ON_ERROR(repeater_firmware_start(&s_runtime), TAG,
                        "Failed to start firmware update runtime");
    ESP_RETURN_ON_ERROR(repeater_web_bridge_start(&s_runtime), TAG, "Failed to start web UI");
    ESP_RETURN_ON_ERROR(repeater_start_status_logging(), TAG,
                        "Failed to start repeater status logging");

    if (repeater_wifi_get_softap_ip_info(&s_runtime, &ap_ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP ready. IP: " IPSTR ", gateway: " IPSTR ", netmask: " IPSTR,
                 IP2STR(&ap_ip_info.ip), IP2STR(&ap_ip_info.gw), IP2STR(&ap_ip_info.netmask));
    }

    s_runtime.started = true;
    ESP_LOGI(TAG, "%s started", PROJECT_DEVICE_NAME);
    return ESP_OK;
}

bool repeater_core_is_upstream_connected(void)
{
    return repeater_wifi_is_upstream_connected(&s_runtime);
}

int repeater_core_get_client_count(void)
{
    return repeater_wifi_get_client_count(&s_runtime);
}
