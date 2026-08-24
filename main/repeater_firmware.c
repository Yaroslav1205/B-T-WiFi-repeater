#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "project_wifi_config.h"
#include "repeater_firmware.h"

#define FIRMWARE_MANIFEST_BUFFER_SIZE 1024
#define FIRMWARE_URL_MAX_LEN 256
#define FIRMWARE_HTTP_READ_BUFFER_SIZE 1024
#define FIRMWARE_HTTP_MAX_REDIRECTS 5
#define FIRMWARE_UPDATE_TASK_STACK_SIZE 8192

static const char *TAG = "RepeaterFirmware";

typedef struct {
    char version[REPEATER_FIRMWARE_VERSION_MAX_LEN];
    char firmware_url[FIRMWARE_URL_MAX_LEN];
} repeater_firmware_manifest_t;

static repeater_runtime_t *s_runtime;
static repeater_firmware_status_t s_status;
static SemaphoreHandle_t s_status_mutex;
static TaskHandle_t s_update_task_handle;

static void repeater_firmware_copy_text(const char *input, char *output, size_t output_size)
{
    const char *value = input != NULL ? input : "";
    size_t length = strlen(value);

    if (length >= output_size) {
        length = output_size - 1;
    }

    memcpy(output, value, length);
    output[length] = '\0';
}

static void repeater_firmware_trim(char *value)
{
    char *start = value;
    char *end;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        ++start;
    }

    if (start != value) {
        memmove(value, start, strlen(start) + 1);
    }

    end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        --end;
    }

    *end = '\0';
}

static void repeater_firmware_status_lock(void)
{
    if (s_status_mutex != NULL) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    }
}

static void repeater_firmware_status_unlock(void)
{
    if (s_status_mutex != NULL) {
        xSemaphoreGive(s_status_mutex);
    }
}

static void repeater_firmware_update_message_locked(bool succeeded, const char *format, ...)
{
    va_list args;

    s_status.last_action_succeeded = succeeded;
    va_start(args, format);
    vsnprintf(s_status.status_message, sizeof(s_status.status_message), format, args);
    va_end(args);
}

static void repeater_firmware_set_message(bool succeeded, const char *format, ...)
{
    va_list args;

    repeater_firmware_status_lock();
    s_status.last_action_succeeded = succeeded;
    va_start(args, format);
    vsnprintf(s_status.status_message, sizeof(s_status.status_message), format, args);
    va_end(args);
    repeater_firmware_status_unlock();
}

static void repeater_firmware_reset_current_version_locked(void)
{
    repeater_firmware_copy_text(PROJECT_FIRMWARE_VERSION,
                                s_status.current_version,
                                sizeof(s_status.current_version));
}

static esp_http_client_config_t repeater_firmware_make_http_config(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = PROJECT_FIRMWARE_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = FIRMWARE_HTTP_MAX_REDIRECTS,
    };

    return config;
}

static int repeater_firmware_read_version_segment(const char **cursor)
{
    int value = 0;
    bool has_digits = false;

    while (**cursor != '\0' && **cursor != '.' && !isdigit((unsigned char)**cursor)) {
        ++(*cursor);
    }

    while (isdigit((unsigned char)**cursor)) {
        has_digits = true;
        value = (value * 10) + (**cursor - '0');
        ++(*cursor);
    }

    while (**cursor != '\0' && **cursor != '.') {
        ++(*cursor);
    }

    if (**cursor == '.') {
        ++(*cursor);
    }

    return has_digits ? value : 0;
}

static int repeater_firmware_compare_versions(const char *left, const char *right)
{
    const char *lhs = left != NULL ? left : "";
    const char *rhs = right != NULL ? right : "";

    for (int index = 0; index < 8; ++index) {
        const int lhs_segment = repeater_firmware_read_version_segment(&lhs);
        const int rhs_segment = repeater_firmware_read_version_segment(&rhs);

        if (lhs_segment != rhs_segment) {
            return lhs_segment < rhs_segment ? -1 : 1;
        }

        if (*lhs == '\0' && *rhs == '\0') {
            return 0;
        }
    }

    return 0;
}

static esp_err_t repeater_firmware_http_get_text(const char *url, char *buffer, size_t buffer_size)
{
    esp_http_client_config_t config = repeater_firmware_make_http_config(url);
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int total_read = 0;

    if (url == NULL || url[0] == '\0' || buffer == NULL || buffer_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "Failed to create HTTP client");

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_fetch_headers(client);
    if (err < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (esp_http_client_get_status_code(client) != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (total_read < (int)buffer_size - 1) {
        int bytes_read = esp_http_client_read(client, buffer + total_read,
                                              (int)buffer_size - 1 - total_read);

        if (bytes_read < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        if (bytes_read == 0) {
            break;
        }

        total_read += bytes_read;
    }

    if (total_read >= (int)buffer_size - 1) {
        int overflow_check = esp_http_client_read(client, buffer, 1);
        if (overflow_check > 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_NO_MEM;
        }
    }

    buffer[total_read] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static bool repeater_firmware_parse_manifest_value(const char *key, const char *value,
                                                   repeater_firmware_manifest_t *manifest)
{
    if (strcmp(key, "version") == 0) {
        repeater_firmware_copy_text(value, manifest->version, sizeof(manifest->version));
        return true;
    }

    if (strcmp(key, "firmware_url") == 0) {
        repeater_firmware_copy_text(value, manifest->firmware_url, sizeof(manifest->firmware_url));
        return true;
    }

    return false;
}

static esp_err_t repeater_firmware_parse_manifest(const char *manifest_text,
                                                  repeater_firmware_manifest_t *out_manifest)
{
    char buffer[FIRMWARE_MANIFEST_BUFFER_SIZE];
    char *saveptr = NULL;
    char *line = NULL;
    bool parsed_anything = false;

    ESP_RETURN_ON_FALSE(manifest_text != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Manifest text is required");
    ESP_RETURN_ON_FALSE(out_manifest != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Manifest output is required");

    memset(out_manifest, 0, sizeof(*out_manifest));
    repeater_firmware_copy_text(manifest_text, buffer, sizeof(buffer));

    for (line = strtok_r(buffer, "\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {
        char *separator = NULL;

        repeater_firmware_trim(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        separator = strchr(line, '=');
        if (separator == NULL) {
            continue;
        }

        *separator = '\0';
        char *key = line;
        char *value = separator + 1;

        repeater_firmware_trim(key);
        repeater_firmware_trim(value);
        parsed_anything |= repeater_firmware_parse_manifest_value(key, value, out_manifest);
    }

    if (!parsed_anything || out_manifest->version[0] == '\0' ||
        out_manifest->firmware_url[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static esp_err_t repeater_firmware_fetch_manifest(repeater_firmware_manifest_t *out_manifest)
{
    char manifest_text[FIRMWARE_MANIFEST_BUFFER_SIZE];

    ESP_RETURN_ON_FALSE(out_manifest != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Manifest output is required");

    ESP_RETURN_ON_FALSE(PROJECT_FIRMWARE_MANIFEST_URL[0] != '\0', ESP_ERR_INVALID_STATE, TAG,
                        "Firmware manifest URL is not configured");
    ESP_RETURN_ON_ERROR(repeater_firmware_http_get_text(PROJECT_FIRMWARE_MANIFEST_URL,
                                                        manifest_text, sizeof(manifest_text)),
                        TAG, "Failed to download firmware manifest");
    ESP_RETURN_ON_ERROR(repeater_firmware_parse_manifest(manifest_text, out_manifest), TAG,
                        "Failed to parse firmware manifest");
    return ESP_OK;
}

static esp_err_t repeater_firmware_check_manifest(repeater_firmware_manifest_t *out_manifest)
{
    repeater_firmware_manifest_t manifest;
    esp_err_t err = repeater_firmware_fetch_manifest(&manifest);

    repeater_firmware_status_lock();
    s_status.check_completed = err == ESP_OK;

    if (err != ESP_OK) {
        s_status.update_available = false;
        s_status.available_version[0] = '\0';
        repeater_firmware_update_message_locked(false,
                                                PROJECT_FIRMWARE_MANIFEST_URL[0] == '\0'
                                                    ? "Firmware server URL is not configured"
                                                    : "Failed to check firmware server");
        repeater_firmware_status_unlock();
        return err;
    }

    repeater_firmware_copy_text(manifest.version, s_status.available_version,
                                sizeof(s_status.available_version));
    s_status.update_available =
        repeater_firmware_compare_versions(s_status.current_version, manifest.version) < 0;

    if (s_status.update_available) {
        repeater_firmware_update_message_locked(true, "New firmware %s is available",
                                                manifest.version);
    } else {
        repeater_firmware_update_message_locked(true, "Current firmware %s is up to date",
                                                s_status.current_version);
    }

    repeater_firmware_status_unlock();

    if (out_manifest != NULL) {
        *out_manifest = manifest;
    }

    return ESP_OK;
}

static esp_err_t repeater_firmware_download_and_apply(const char *firmware_url)
{
    esp_http_client_config_t config = repeater_firmware_make_http_config(firmware_url);
    esp_http_client_handle_t client = NULL;
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    char read_buffer[FIRMWARE_HTTP_READ_BUFFER_SIZE];
    esp_err_t err = ESP_FAIL;
    bool ota_started = false;

    ESP_RETURN_ON_FALSE(firmware_url != NULL && firmware_url[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "Firmware URL is required");

    update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_RETURN_ON_FALSE(update_partition != NULL, ESP_FAIL, TAG,
                        "No OTA partition available");

    client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "Failed to create HTTP client");

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = esp_http_client_fetch_headers(client);
    if (err < 0) {
        err = ESP_FAIL;
        goto cleanup;
    }

    if (esp_http_client_get_status_code(client) != 200) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        goto cleanup;
    }

    ota_started = true;

    while (true) {
        const int bytes_read = esp_http_client_read(client, read_buffer, sizeof(read_buffer));

        if (bytes_read < 0) {
            err = ESP_FAIL;
            goto cleanup;
        }

        if (bytes_read == 0) {
            break;
        }

        err = esp_ota_write(ota_handle, read_buffer, (size_t)bytes_read);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    err = esp_ota_end(ota_handle);
    ota_started = false;
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = esp_ota_set_boot_partition(update_partition);

cleanup:
    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    if (ota_started) {
        esp_ota_abort(ota_handle);
    }

    return err;
}

static void repeater_firmware_update_task(void *arg)
{
    (void)arg;

    repeater_firmware_manifest_t manifest;
    esp_err_t err;

    repeater_firmware_set_message(true, "Checking firmware server before update");
    err = repeater_firmware_check_manifest(&manifest);
    if (err != ESP_OK) {
        goto cleanup;
    }

    repeater_firmware_status_lock();
    if (!s_status.update_available) {
        repeater_firmware_update_message_locked(true, "Latest firmware is already installed");
        repeater_firmware_status_unlock();
        goto cleanup;
    }

    s_status.update_in_progress = true;
    repeater_firmware_update_message_locked(true, "Downloading firmware %s", manifest.version);
    repeater_firmware_status_unlock();

    ESP_LOGI(TAG, "Starting OTA update from %s", manifest.firmware_url);
    err = repeater_firmware_download_and_apply(manifest.firmware_url);
    if (err != ESP_OK) {
        repeater_firmware_status_lock();
        s_status.update_in_progress = false;
        repeater_firmware_update_message_locked(false, "Firmware update failed");
        repeater_firmware_status_unlock();
        ESP_LOGE(TAG, "Firmware update failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    repeater_firmware_status_lock();
    s_status.update_in_progress = false;
    s_status.update_available = false;
    repeater_firmware_copy_text(manifest.version, s_status.available_version,
                                sizeof(s_status.available_version));
    repeater_firmware_update_message_locked(true, "Firmware %s installed, rebooting",
                                            manifest.version);
    repeater_firmware_status_unlock();

    ESP_LOGI(TAG, "Firmware update installed successfully, rebooting soon");
    vTaskDelay(pdMS_TO_TICKS(PROJECT_FIRMWARE_REBOOT_DELAY_MS));
    esp_restart();

cleanup:
    s_update_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t repeater_firmware_start(repeater_runtime_t *runtime)
{
    (void)runtime;

    if (s_status_mutex == NULL) {
        s_status_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_status_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                            "Failed to create firmware status mutex");
    }

    s_runtime = runtime;

    repeater_firmware_status_lock();
    memset(&s_status, 0, sizeof(s_status));
    repeater_firmware_reset_current_version_locked();
    s_status.manifest_configured = PROJECT_FIRMWARE_MANIFEST_URL[0] != '\0';
    repeater_firmware_update_message_locked(true,
                                            s_status.manifest_configured
                                                ? "Firmware server is configured"
                                                : "Configure PROJECT_FIRMWARE_MANIFEST_URL to enable updates");
    repeater_firmware_status_unlock();
    return ESP_OK;
}

void repeater_firmware_get_status(repeater_firmware_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    repeater_firmware_status_lock();
    repeater_firmware_reset_current_version_locked();
    *out_status = s_status;
    repeater_firmware_status_unlock();
}

esp_err_t repeater_firmware_check_now(repeater_firmware_status_t *out_status)
{
    esp_err_t err;

    if (s_update_task_handle != NULL) {
        repeater_firmware_set_message(false, "Firmware update is already in progress");
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = repeater_firmware_check_manifest(NULL);
    }

    if (out_status != NULL) {
        repeater_firmware_get_status(out_status);
    }

    return err;
}

esp_err_t repeater_firmware_start_update(void)
{
    BaseType_t task_result;

    if (PROJECT_FIRMWARE_MANIFEST_URL[0] == '\0') {
        repeater_firmware_set_message(false, "Firmware server URL is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_update_task_handle != NULL) {
        repeater_firmware_set_message(false, "Firmware update is already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    task_result = xTaskCreate(repeater_firmware_update_task, "firmware_update",
                              FIRMWARE_UPDATE_TASK_STACK_SIZE, NULL, 1, &s_update_task_handle);
    if (task_result != pdPASS) {
        s_update_task_handle = NULL;
        repeater_firmware_set_message(false, "Failed to start firmware update task");
        return ESP_FAIL;
    }

    repeater_firmware_status_lock();
    s_status.update_in_progress = true;
    repeater_firmware_update_message_locked(true, "Firmware update started");
    repeater_firmware_status_unlock();
    return ESP_OK;
}
