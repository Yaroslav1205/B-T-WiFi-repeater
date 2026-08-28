#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "project_wifi_config.h"
#include "status_led.h"

static const char *TAG = "StatusLED";
static status_led_context_t s_context;
static TaskHandle_t s_status_led_task_handle;
static const TickType_t STATUS_LED_STARTUP_BLINK_ON_TICKS = pdMS_TO_TICKS(40);
static const TickType_t STATUS_LED_STARTUP_BLINK_OFF_TICKS = pdMS_TO_TICKS(60);
static const int STATUS_LED_STARTUP_BLINK_COUNT = 10;

static void status_led_set(bool on)
{
    gpio_set_level(PROJECT_STATUS_LED_GPIO,
                   on ? PROJECT_STATUS_LED_ACTIVE_LEVEL : !PROJECT_STATUS_LED_ACTIVE_LEVEL);
}

static void status_led_run_startup_pattern(void)
{
    for (int i = 0; i < STATUS_LED_STARTUP_BLINK_COUNT; ++i) {
        status_led_set(true);
        vTaskDelay(STATUS_LED_STARTUP_BLINK_ON_TICKS);
        status_led_set(false);
        vTaskDelay(STATUS_LED_STARTUP_BLINK_OFF_TICKS);
    }
}

static void status_led_task(void *arg)
{
    while (true) {
        const bool upstream_connected = s_context.is_upstream_connected();
        const int client_count = s_context.get_client_count();
        const int blink_count = client_count > 0 ? client_count : 0;
        const bool base_on = !upstream_connected;

        if (blink_count == 0) {
            status_led_set(base_on);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (int i = 0; i < blink_count; ++i) {
            status_led_set(!base_on);
            vTaskDelay(pdMS_TO_TICKS(180));
            status_led_set(base_on);
            vTaskDelay(pdMS_TO_TICKS(220));
        }

        vTaskDelay(pdMS_TO_TICKS(1200));
    }
}

esp_err_t status_led_start(const status_led_context_t *context)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PROJECT_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    BaseType_t task_result;

    if (!PROJECT_STATUS_LED_ENABLED) {
        ESP_LOGI(TAG, "Status LED disabled in config");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(context != NULL, ESP_ERR_INVALID_ARG, TAG, "Context is required");
    ESP_RETURN_ON_FALSE(context->is_upstream_connected != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Upstream status callback is required");
    ESP_RETURN_ON_FALSE(context->get_client_count != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Client count callback is required");
    ESP_RETURN_ON_FALSE(s_status_led_task_handle == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "Status LED already started");

    s_context = *context;

    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to configure status LED GPIO");
    status_led_set(false);
    status_led_run_startup_pattern();

    task_result = xTaskCreate(status_led_task, "status_led_task", 2048, NULL, 1,
                              &s_status_led_task_handle);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_FAIL, TAG,
                        "Failed to create status LED task");

    ESP_LOGI(TAG, "Status LED started on GPIO%d, active level=%d",
             PROJECT_STATUS_LED_GPIO, PROJECT_STATUS_LED_ACTIVE_LEVEL);
    return ESP_OK;
}
