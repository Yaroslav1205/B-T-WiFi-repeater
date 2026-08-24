#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool (*is_upstream_connected)(void);
    int (*get_client_count)(void);
} status_led_context_t;

esp_err_t status_led_start(const status_led_context_t *context);
