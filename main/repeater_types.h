#pragma once

#include "repeater_settings.h"

typedef struct {
    char mac[REPEATER_MAC_STRING_LEN];
    int rssi;
    char description[REPEATER_DEVICE_DESCRIPTION_MAX_LEN + 1];
    bool is_connected;
    int64_t first_seen_epoch;
    int64_t last_seen_epoch;
} repeater_client_info_t;

#define REPEATER_FIRMWARE_VERSION_MAX_LEN 32
#define REPEATER_FIRMWARE_MESSAGE_MAX_LEN 160

typedef struct {
    char current_version[REPEATER_FIRMWARE_VERSION_MAX_LEN];
    char available_version[REPEATER_FIRMWARE_VERSION_MAX_LEN];
    char status_message[REPEATER_FIRMWARE_MESSAGE_MAX_LEN];
    bool manifest_configured;
    bool check_completed;
    bool update_available;
    bool update_in_progress;
    bool last_action_succeeded;
} repeater_firmware_status_t;
