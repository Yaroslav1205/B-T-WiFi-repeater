#pragma once

#include "esp_err.h"
#include "repeater_runtime.h"
#include "repeater_types.h"

esp_err_t repeater_firmware_start(repeater_runtime_t *runtime);
void repeater_firmware_get_status(repeater_firmware_status_t *out_status);
esp_err_t repeater_firmware_check_now(repeater_firmware_status_t *out_status);
esp_err_t repeater_firmware_start_update(void);
