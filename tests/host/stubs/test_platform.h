#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_VERSION 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define ESP_ERR_NVS_INVALID_LENGTH 5
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { esp_err_t e_ = (expr); if (e_ != ESP_OK) return e_; } while (0)
#define ESP_RETURN_ON_FALSE(expr, err, tag, ...) do { if (!(expr)) return (err); } while (0)

typedef enum {
    WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK,
    WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK
} wifi_auth_mode_t;
esp_err_t esp_wifi_set_max_tx_power(int8_t power);

typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_get_str(nvs_handle_t, const char *, char *, size_t *);
esp_err_t nvs_set_str(nvs_handle_t, const char *, const char *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_i8(nvs_handle_t, const char *, int8_t *);
esp_err_t nvs_set_i8(nvs_handle_t, const char *, int8_t);

typedef unsigned int TickType_t;
#define pdMS_TO_TICKS(ms) ((ms) / 10)
void vTaskDelay(TickType_t ticks);
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
esp_err_t gpio_config(const gpio_config_t *);
int gpio_get_level(int pin);
esp_err_t gpio_set_level(int pin, int level);
