#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t repeater_core_start(void);
bool repeater_core_is_upstream_connected(void);
int repeater_core_get_client_count(void);
