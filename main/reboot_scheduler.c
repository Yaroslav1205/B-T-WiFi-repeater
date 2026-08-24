#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "project_wifi_config.h"
#include "reboot_scheduler.h"
#include "repeater_settings.h"

typedef struct {
    TaskHandle_t task_handle;
    bool started;
    bool time_available_logged;
    int last_reboot_year;
    int last_reboot_yday;
} reboot_scheduler_runtime_t;

static const char *TAG = "RebootScheduler";
static reboot_scheduler_runtime_t s_runtime = {
    .last_reboot_year = -1,
    .last_reboot_yday = -1,
};

static void reboot_scheduler_time_sync_cb(struct timeval *tv)
{
    time_t now;
    struct tm timeinfo;
    char time_buffer[32];

    (void)tv;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "System time synchronized: %s", time_buffer);
}

static bool reboot_scheduler_get_local_time(struct tm *timeinfo)
{
    time_t now;

    time(&now);
    localtime_r(&now, timeinfo);
    return timeinfo->tm_year >= (2024 - 1900);
}

static void reboot_scheduler_task(void *arg)
{
    (void)arg;

    while (true) {
        const repeater_auto_reboot_config_t config = repeater_settings_get_auto_reboot_config();
        struct tm timeinfo;

        if (!config.enabled) {
            s_runtime.time_available_logged = false;
            s_runtime.last_reboot_year = -1;
            s_runtime.last_reboot_yday = -1;
            vTaskDelay(pdMS_TO_TICKS(PROJECT_REBOOT_CHECK_INTERVAL_MS));
            continue;
        }

        if (!reboot_scheduler_get_local_time(&timeinfo)) {
            if (!s_runtime.time_available_logged) {
                ESP_LOGW(TAG, "Auto reboot is enabled for %02u:%02u, waiting for NTP time sync",
                         config.hour, config.minute);
                s_runtime.time_available_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(PROJECT_REBOOT_CHECK_INTERVAL_MS));
            continue;
        }

        if (s_runtime.time_available_logged) {
            ESP_LOGI(TAG, "Auto reboot scheduler now has valid time");
            s_runtime.time_available_logged = false;
        }

        if (timeinfo.tm_year != s_runtime.last_reboot_year ||
            timeinfo.tm_yday != s_runtime.last_reboot_yday) {
            if (timeinfo.tm_hour == config.hour && timeinfo.tm_min == config.minute) {
                s_runtime.last_reboot_year = timeinfo.tm_year;
                s_runtime.last_reboot_yday = timeinfo.tm_yday;
                ESP_LOGW(TAG, "Executing scheduled protective reboot at %02u:%02u",
                         config.hour, config.minute);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PROJECT_REBOOT_CHECK_INTERVAL_MS));
    }
}

esp_err_t reboot_scheduler_start(void)
{
    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(PROJECT_SNTP_SERVER);
    BaseType_t task_result;

    ESP_RETURN_ON_FALSE(!s_runtime.started, ESP_ERR_INVALID_STATE, TAG,
                        "Reboot scheduler already started");

    setenv("TZ", PROJECT_TIMEZONE_TZ, 1);
    tzset();

    sntp_config.sync_cb = reboot_scheduler_time_sync_cb;
    sntp_config.wait_for_sync = false;
    sntp_config.renew_servers_after_new_IP = true;
    sntp_config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;

    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp_config), TAG,
                        "Failed to initialize SNTP");

    task_result = xTaskCreate(reboot_scheduler_task, "reboot_scheduler", 4096, NULL, 1,
                              &s_runtime.task_handle);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_FAIL, TAG,
                        "Failed to create reboot scheduler task");

    ESP_LOGI(TAG, "Auto reboot scheduler started. NTP server=%s TZ=%s",
             PROJECT_SNTP_SERVER, PROJECT_TIMEZONE_TZ);
    s_runtime.started = true;
    return ESP_OK;
}
