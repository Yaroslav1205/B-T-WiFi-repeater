idf_component_register(SRCS
                    "app_main.c"
                    "repeater_firmware.c"
                    "reboot_scheduler.c"
                    "repeater_core.c"
                    "repeater_web_bridge.c"
                    "repeater_settings.c"
                    "repeater_wifi.c"
                    "router_web.c"
                    "status_led.c"
                    PRIV_REQUIRES app_update esp_event esp_http_client esp_netif esp_wifi nvs_flash esp_driver_gpio esp_http_server esp_driver_tsens mbedtls
                    INCLUDE_DIRS ".")
