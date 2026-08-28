#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "reboot_scheduler.h"
#include "repeater_core.h"
#include "repeater_settings.h"
#include "startup_factory_reset.h"
#include "status_led.h"

static void storage_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    const status_led_context_t led_context = {
        .is_upstream_connected = repeater_core_is_upstream_connected,
        .get_client_count = repeater_core_get_client_count,
    };

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    storage_init();
    ESP_ERROR_CHECK(startup_factory_reset_check());
    ESP_ERROR_CHECK(repeater_settings_init());
    ESP_ERROR_CHECK(repeater_core_start());
    ESP_ERROR_CHECK(reboot_scheduler_start());
    ESP_ERROR_CHECK(status_led_start(&led_context));

    vTaskDelete(NULL);
}
