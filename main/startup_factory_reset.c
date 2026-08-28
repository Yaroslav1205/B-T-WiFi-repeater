#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "project_wifi_config.h"
#include "repeater_settings.h"
#include "startup_factory_reset.h"

static const char *TAG = "StartupReset";

static void startup_set_status_led(bool on)
{
    if (!PROJECT_STATUS_LED_ENABLED) {
        return;
    }

    gpio_set_level(PROJECT_STATUS_LED_GPIO,
                   on ? PROJECT_STATUS_LED_ACTIVE_LEVEL : !PROJECT_STATUS_LED_ACTIVE_LEVEL);
}

static bool startup_is_boot_button_pressed(void)
{
    return gpio_get_level(PROJECT_BOOT_BUTTON_GPIO) == PROJECT_BOOT_BUTTON_ACTIVE_LEVEL;
}

static esp_err_t startup_configure_status_led(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PROJECT_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    if (!PROJECT_STATUS_LED_ENABLED) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to configure status LED GPIO");
    startup_set_status_led(false);
    return ESP_OK;
}

static esp_err_t startup_configure_boot_button(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PROJECT_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = PROJECT_BOOT_BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = PROJECT_BOOT_BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Failed to configure BOOT button GPIO");
    return ESP_OK;
}

static void startup_blink_factory_reset_complete(void)
{
    startup_set_status_led(false);
    vTaskDelay(pdMS_TO_TICKS(PROJECT_FACTORY_RESET_BLINK_MS));
    startup_set_status_led(true);
    vTaskDelay(pdMS_TO_TICKS(PROJECT_FACTORY_RESET_BLINK_MS));
    startup_set_status_led(false);
}

esp_err_t startup_factory_reset_check(void)
{
    const TickType_t poll_ticks = pdMS_TO_TICKS(PROJECT_FACTORY_RESET_POLL_MS);
    uint32_t elapsed_ms = 0;

    if (!PROJECT_BOOT_BUTTON_ENABLED) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(startup_configure_boot_button(), TAG,
                        "Failed to prepare BOOT button for startup factory reset check");
    ESP_RETURN_ON_ERROR(startup_configure_status_led(), TAG,
                        "Failed to prepare status LED for startup factory reset check");

    if (!startup_is_boot_button_pressed()) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "BOOT button pressed on startup, hold for %u ms to factory reset",
             (unsigned int)PROJECT_FACTORY_RESET_HOLD_MS);
    startup_set_status_led(true);

    while (elapsed_ms < PROJECT_FACTORY_RESET_HOLD_MS) {
        if (!startup_is_boot_button_pressed()) {
            startup_set_status_led(false);
            ESP_LOGI(TAG, "BOOT button released before factory reset timeout, continuing normal startup");
            return ESP_OK;
        }

        vTaskDelay(poll_ticks);
        elapsed_ms += PROJECT_FACTORY_RESET_POLL_MS;
    }

    if (!startup_is_boot_button_pressed()) {
        startup_set_status_led(false);
        ESP_LOGI(TAG, "BOOT button released at timeout boundary, continuing normal startup");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Factory reset requested from BOOT button");
    ESP_RETURN_ON_ERROR(repeater_settings_factory_reset(), TAG,
                        "Failed to erase saved settings during startup factory reset");
    startup_blink_factory_reset_complete();
    ESP_LOGI(TAG, "Factory reset complete, continuing normal startup");
    return ESP_OK;
}
