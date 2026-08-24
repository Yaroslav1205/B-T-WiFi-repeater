#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "project_wifi_config.h"
#include "router_web.h"

static const char *TAG = "RouterWeb";
static router_web_context_t s_context_storage;
static const router_web_context_t *s_context = &s_context_storage;

#define ROUTER_WEB_AUTH_REALM PROJECT_DEVICE_NAME
#define ROUTER_WEB_AUTH_RAW_LEN \
    (REPEATER_WEB_AUTH_USERNAME_MAX_LEN + 1 + REPEATER_WEB_AUTH_PASSWORD_MAX_LEN)
#define ROUTER_WEB_AUTH_B64_LEN ((((ROUTER_WEB_AUTH_RAW_LEN) + 2) / 3) * 4)
#define ROUTER_WEB_AUTH_HEADER_LEN ((sizeof("Basic ") - 1) + ROUTER_WEB_AUTH_B64_LEN + 1)
#define ROUTER_WEB_OPTIONS_BUFFER_SIZE 1024
#define ROUTER_WEB_THEME_OPTIONS_BUFFER_SIZE 256
#define ROUTER_WEB_SOFTAP_AUTH_OPTIONS_BUFFER_SIZE 256
#define ROUTER_WEB_CLIENT_ROW_ESTIMATE 384
#define ROUTER_WEB_CLIENTS_BUFFER_BASE_SIZE 256
#define ROUTER_WEB_HTML_BUFFER_BASE_SIZE 24576
#define ROUTER_WEB_MAX_CLIENT_ROWS REPEATER_CLIENT_HISTORY_MAX_ENTRIES
#define ROUTER_WEB_HTTP_STACK_SIZE 16384
#define ROUTER_WEB_HTML_ESCAPE_EXPANSION_MAX 6
#define ROUTER_WEB_ESCAPED_BUFFER_SIZE(max_plain_len) \
    (((max_plain_len) * ROUTER_WEB_HTML_ESCAPE_EXPANSION_MAX) + 1)
#define ROUTER_WEB_ESCAPED_SSID_BUFFER_SIZE \
    ROUTER_WEB_ESCAPED_BUFFER_SIZE(REPEATER_WIFI_SSID_MAX_LEN)
#define ROUTER_WEB_ESCAPED_WIFI_PASSWORD_BUFFER_SIZE \
    ROUTER_WEB_ESCAPED_BUFFER_SIZE(REPEATER_WIFI_PASSWORD_MAX_LEN)
#define ROUTER_WEB_ESCAPED_WEB_USERNAME_BUFFER_SIZE \
    ROUTER_WEB_ESCAPED_BUFFER_SIZE(REPEATER_WEB_AUTH_USERNAME_MAX_LEN)
#define ROUTER_WEB_ESCAPED_WEB_PASSWORD_BUFFER_SIZE \
    ROUTER_WEB_ESCAPED_BUFFER_SIZE(REPEATER_WEB_AUTH_PASSWORD_MAX_LEN)
#define ROUTER_WEB_ESCAPED_DEVICE_DESCRIPTION_BUFFER_SIZE \
    ROUTER_WEB_ESCAPED_BUFFER_SIZE(REPEATER_DEVICE_DESCRIPTION_MAX_LEN)
#define ROUTER_WEB_ESCAPED_IP_TEXT_BUFFER_SIZE ROUTER_WEB_ESCAPED_BUFFER_SIZE(31)
#define ROUTER_WEB_ESCAPED_FIRMWARE_VERSION_BUFFER_SIZE \
    ROUTER_WEB_ESCAPED_BUFFER_SIZE(REPEATER_FIRMWARE_VERSION_MAX_LEN)
#define ROUTER_WEB_ESCAPED_FIRMWARE_MESSAGE_BUFFER_SIZE \
    ROUTER_WEB_ESCAPED_BUFFER_SIZE(REPEATER_FIRMWARE_MESSAGE_MAX_LEN)
#define ROUTER_WEB_DEVICE_FORM_BODY_SIZE 192
#define ROUTER_WEB_SETTINGS_FORM_BODY_SIZE 1536
#define ROUTER_WEB_QUERY_BUFFER_SIZE 128
#define ROUTER_WEB_STRINGIFY_INNER(value) #value
#define ROUTER_WEB_STRINGIFY(value) ROUTER_WEB_STRINGIFY_INNER(value)
#define ROUTER_WEB_DEVICE_DESCRIPTION_MAX_LEN_STR \
    ROUTER_WEB_STRINGIFY(REPEATER_DEVICE_DESCRIPTION_MAX_LEN)
#define ROUTER_WEB_WIFI_SSID_MAX_LEN_STR \
    ROUTER_WEB_STRINGIFY(REPEATER_WIFI_SSID_MAX_LEN)
#define ROUTER_WEB_WIFI_PASSWORD_MAX_LEN_STR \
    ROUTER_WEB_STRINGIFY(REPEATER_WIFI_PASSWORD_MAX_LEN)
#define ROUTER_WEB_AUTH_USERNAME_MAX_LEN_STR \
    ROUTER_WEB_STRINGIFY(REPEATER_WEB_AUTH_USERNAME_MAX_LEN)
#define ROUTER_WEB_AUTH_PASSWORD_MAX_LEN_STR \
    ROUTER_WEB_STRINGIFY(REPEATER_WEB_AUTH_PASSWORD_MAX_LEN)

static char s_expected_auth_header[ROUTER_WEB_AUTH_HEADER_LEN];

static size_t base64_encode(const uint8_t *input, size_t input_len, char *output, size_t output_len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t in_index = 0;
    size_t out_index = 0;

    while (in_index < input_len) {
        size_t remaining = input_len - in_index;
        uint32_t octet_a = input[in_index++];
        uint32_t octet_b = remaining > 1 ? input[in_index++] : 0;
        uint32_t octet_c = remaining > 2 ? input[in_index++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        if (out_index + 4 >= output_len) {
            return 0;
        }

        output[out_index++] = alphabet[(triple >> 18) & 0x3F];
        output[out_index++] = alphabet[(triple >> 12) & 0x3F];
        output[out_index++] = remaining > 1 ? alphabet[(triple >> 6) & 0x3F] : '=';
        output[out_index++] = remaining > 2 ? alphabet[triple & 0x3F] : '=';
    }

    output[out_index] = '\0';
    return out_index;
}

static esp_err_t build_expected_auth_header(void)
{
    char credentials[ROUTER_WEB_AUTH_RAW_LEN + 1];
    char encoded[ROUTER_WEB_AUTH_B64_LEN + 1];
    const repeater_web_auth_config_t *web_auth_config =
        s_context->get_web_auth_config != NULL ? s_context->get_web_auth_config() : NULL;
    const char *username = web_auth_config != NULL ? web_auth_config->username : PROJECT_WEB_AUTH_USERNAME;
    const char *password = web_auth_config != NULL ? web_auth_config->password : PROJECT_WEB_AUTH_PASSWORD;
    int written = snprintf(credentials, sizeof(credentials), "%s:%s", username, password);

    if (written < 0 || written >= (int)sizeof(credentials)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (base64_encode((const uint8_t *)credentials, strlen(credentials), encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(s_expected_auth_header, sizeof(s_expected_auth_header), "Basic %s", encoded);
    if (written < 0 || written >= (int)sizeof(s_expected_auth_header)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t send_auth_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"" ROUTER_WEB_AUTH_REALM "\"");
    return httpd_resp_send(req, "Authentication required", HTTPD_RESP_USE_STRLEN);
}

static bool is_request_authorized(httpd_req_t *req)
{
    char auth_header[sizeof(s_expected_auth_header)];

    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    return strcmp(auth_header, s_expected_auth_header) == 0;
}

static esp_err_t append_select_option(char *buffer, size_t buffer_size, size_t *offset,
                                      const char *value, const char *selected_value,
                                      const char *label)
{
    int written = snprintf(buffer + *offset, buffer_size - *offset,
                           "<option value=\"%s\" %s>%s</option>",
                           value,
                           strcmp(selected_value, value) == 0 ? "selected" : "",
                           label);

    if (written < 0 || (size_t)written >= buffer_size - *offset) {
        return ESP_ERR_NO_MEM;
    }

    *offset += (size_t)written;
    return ESP_OK;
}

static void format_client_timestamp(int64_t epoch_seconds, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    if (epoch_seconds <= 0) {
        snprintf(output, output_size, "Not synced yet");
        return;
    }

    time_t timestamp = (time_t)epoch_seconds;
    struct tm time_info;

    if (localtime_r(&timestamp, &time_info) == NULL ||
        strftime(output, output_size, "%Y-%m-%d %H:%M:%S", &time_info) == 0) {
        snprintf(output, output_size, "Unavailable");
    }
}

static int hex_char_to_int(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static void url_decode_in_place(char *value)
{
    char *read_ptr = value;
    char *write_ptr = value;

    while (*read_ptr != '\0') {
        if (*read_ptr == '%' && read_ptr[1] != '\0' && read_ptr[2] != '\0') {
            int high = hex_char_to_int(read_ptr[1]);
            int low = hex_char_to_int(read_ptr[2]);

            if (high >= 0 && low >= 0) {
                *write_ptr++ = (char)((high << 4) | low);
                read_ptr += 3;
                continue;
            }
        }

        *write_ptr++ = *read_ptr == '+' ? ' ' : *read_ptr;
        ++read_ptr;
    }

    *write_ptr = '\0';
}

static esp_err_t read_form_body(httpd_req_t *req, char *body, size_t body_size)
{
    int total_received = 0;

    if (req->content_len <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (req->content_len >= (int)body_size) {
        return ESP_ERR_NO_MEM;
    }

    while (total_received < req->content_len) {
        int received = httpd_req_recv(req, body + total_received, req->content_len - total_received);

        if (received <= 0) {
            return ESP_FAIL;
        }

        total_received += received;
    }

    body[total_received] = '\0';
    return ESP_OK;
}

static esp_err_t read_form_value(const char *body, const char *field_name,
                                 char *value, size_t value_size)
{
    esp_err_t err;
    char *encoded_value = NULL;
    size_t encoded_value_size;

    if (value == NULL || value_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Form values arrive URL-encoded, so reserve space for the worst case
    // where every input byte becomes a 3-byte %HH sequence before decoding.
    encoded_value_size = ((value_size - 1) * 3) + 1;
    encoded_value = calloc(1, encoded_value_size);
    if (encoded_value == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = httpd_query_key_value(body, field_name, encoded_value, encoded_value_size);

    if (err != ESP_OK) {
        free(encoded_value);
        return err;
    }

    url_decode_in_place(encoded_value);
    if (strlen(encoded_value) >= value_size) {
        free(encoded_value);
        return ESP_ERR_NO_MEM;
    }

    snprintf(value, value_size, "%s", encoded_value);
    free(encoded_value);
    return ESP_OK;
}

static esp_err_t html_escape_string(const char *input, char *output, size_t output_size)
{
    const char *source = input != NULL ? input : "";
    size_t offset = 0;

    while (*source != '\0') {
        const char *replacement = NULL;
        size_t replacement_len = 1;

        switch (*source) {
        case '&':
            replacement = "&amp;";
            replacement_len = sizeof("&amp;") - 1;
            break;
        case '<':
            replacement = "&lt;";
            replacement_len = sizeof("&lt;") - 1;
            break;
        case '>':
            replacement = "&gt;";
            replacement_len = sizeof("&gt;") - 1;
            break;
        case '"':
            replacement = "&quot;";
            replacement_len = sizeof("&quot;") - 1;
            break;
        case '\'':
            replacement = "&#39;";
            replacement_len = sizeof("&#39;") - 1;
            break;
        default:
            break;
        }

        if (offset + replacement_len >= output_size) {
            return ESP_ERR_NO_MEM;
        }

        if (replacement != NULL) {
            memcpy(output + offset, replacement, replacement_len);
            offset += replacement_len;
        } else {
            output[offset++] = *source;
        }

        ++source;
    }

    output[offset] = '\0';
    return ESP_OK;
}

static void format_temperature_text(float celsius, char *output, size_t output_size,
                                    const char *fallback_text)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    if (isnan(celsius)) {
        snprintf(output, output_size, "%s",
                 fallback_text != NULL ? fallback_text : "Unavailable");
        return;
    }

    const int scaled = celsius >= 0.0f
        ? (int)(celsius * 10.0f + 0.5f)
        : (int)(celsius * 10.0f - 0.5f);
    const int whole = scaled / 10;
    const int fraction = abs(scaled % 10);

    snprintf(output, output_size, "%d.%d C", whole, fraction);
}

static esp_err_t parse_hhmm_time(const char *value, uint8_t *hour, uint8_t *minute)
{
    if (strlen(value) != 5 || value[2] != ':' ||
        value[0] < '0' || value[0] > '9' ||
        value[1] < '0' || value[1] > '9' ||
        value[3] < '0' || value[3] > '9' ||
        value[4] < '0' || value[4] > '9') {
        return ESP_ERR_INVALID_ARG;
    }

    *hour = (uint8_t)((value[0] - '0') * 10 + (value[1] - '0'));
    *minute = (uint8_t)((value[3] - '0') * 10 + (value[4] - '0'));

    if (*hour >= 24 || *minute >= 60) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t web_root_get_handler(httpd_req_t *req)
{
    char *options_html = NULL;
    char *theme_options_html = NULL;
    char *softap_auth_options_html = NULL;
    char *clients_html = NULL;
    char *html = NULL;
    char notice_query[ROUTER_WEB_QUERY_BUFFER_SIZE];
    char notice_value[32];
    char notice_html[512];
    char escaped_station_ssid[ROUTER_WEB_ESCAPED_SSID_BUFFER_SIZE];
    char escaped_station_password[ROUTER_WEB_ESCAPED_WIFI_PASSWORD_BUFFER_SIZE];
    char escaped_backup_station_ssid[ROUTER_WEB_ESCAPED_SSID_BUFFER_SIZE];
    char escaped_backup_station_password[ROUTER_WEB_ESCAPED_WIFI_PASSWORD_BUFFER_SIZE];
    char escaped_softap_ssid[ROUTER_WEB_ESCAPED_SSID_BUFFER_SIZE];
    char escaped_softap_password[ROUTER_WEB_ESCAPED_WIFI_PASSWORD_BUFFER_SIZE];
    char escaped_web_username[ROUTER_WEB_ESCAPED_WEB_USERNAME_BUFFER_SIZE];
    char escaped_web_password[ROUTER_WEB_ESCAPED_WEB_PASSWORD_BUFFER_SIZE];
    char escaped_active_upstream_ssid[ROUTER_WEB_ESCAPED_SSID_BUFFER_SIZE];
    char escaped_active_upstream_ip[ROUTER_WEB_ESCAPED_IP_TEXT_BUFFER_SIZE];
    char escaped_firmware_current_version[ROUTER_WEB_ESCAPED_FIRMWARE_VERSION_BUFFER_SIZE];
    char escaped_firmware_available_version[ROUTER_WEB_ESCAPED_FIRMWARE_VERSION_BUFFER_SIZE];
    char escaped_firmware_status_message[ROUTER_WEB_ESCAPED_FIRMWARE_MESSAGE_BUFFER_SIZE];
    char chip_temperature[24];
    char reboot_time[6];
    char reboot_summary[32];
    char upstream_ip_text[32];
    size_t offset = 0;
    size_t theme_offset = 0;
    size_t softap_auth_offset = 0;
    size_t clients_offset = 0;
    size_t clients_html_capacity = ROUTER_WEB_CLIENTS_BUFFER_BASE_SIZE;
    size_t html_capacity = ROUTER_WEB_HTML_BUFFER_BASE_SIZE;
    repeater_client_info_t *clients = NULL;
    const char *softap_auth_token = s_context->get_softap_auth_token != NULL
        ? s_context->get_softap_auth_token()
        : "";
    size_t client_count = 0;
    const repeater_auto_reboot_config_t auto_reboot_config =
        s_context->get_auto_reboot_config();
    const repeater_station_config_t *station_config =
        s_context->get_station_config != NULL ? s_context->get_station_config() : NULL;
    const repeater_station_config_t *backup_station_config =
        s_context->get_backup_station_config != NULL ? s_context->get_backup_station_config() : NULL;
    const repeater_softap_config_t *softap_config =
        s_context->get_softap_config != NULL ? s_context->get_softap_config() : NULL;
    const repeater_web_auth_config_t *web_auth_config =
        s_context->get_web_auth_config != NULL ? s_context->get_web_auth_config() : NULL;
    const bool using_backup_upstream =
        s_context->is_using_backup_upstream != NULL && s_context->is_using_backup_upstream();
    const char *active_upstream_role = using_backup_upstream ? "Backup" : "Primary";
    const char *active_upstream_ssid =
        s_context->get_active_upstream_ssid != NULL ? s_context->get_active_upstream_ssid() : "";
    repeater_firmware_status_t firmware_status = { 0 };
    const char *notice_title = NULL;
    const char *notice_message = NULL;
    const unsigned int reboot_hour = auto_reboot_config.hour < 24 ? auto_reboot_config.hour : 0;
    const unsigned int reboot_minute = auto_reboot_config.minute < 60 ? auto_reboot_config.minute : 0;
    esp_err_t response_err = ESP_OK;

    if (!is_request_authorized(req)) {
        ESP_LOGW(TAG, "Unauthorized access to root page");
        return send_auth_challenge(req);
    }

    notice_html[0] = '\0';
    if (httpd_req_get_url_query_str(req, notice_query, sizeof(notice_query)) == ESP_OK &&
        httpd_query_key_value(notice_query, "notice", notice_value, sizeof(notice_value)) == ESP_OK) {
        if (strcmp(notice_value, "settings-saved") == 0) {
            notice_title = "Settings saved";
            notice_message = "All changes were saved successfully.";
        } else if (strcmp(notice_value, "device-saved") == 0) {
            notice_title = "Client saved";
            notice_message = "The device description was saved successfully.";
        } else if (strcmp(notice_value, "device-removed") == 0) {
            notice_title = "Client updated";
            notice_message = "The device description was removed successfully.";
        }
    }

    if (notice_title != NULL && notice_message != NULL) {
        int notice_written = snprintf(
            notice_html, sizeof(notice_html),
            "<div class=\"panel notice-banner\">"
            "<strong>%s</strong>"
            "<p>%s</p>"
            "</div>",
            notice_title, notice_message);

        if (notice_written < 0 || notice_written >= (int)sizeof(notice_html)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Notice buffer too small");
        }
    }

    options_html = calloc(1, ROUTER_WEB_OPTIONS_BUFFER_SIZE);
    theme_options_html = calloc(1, ROUTER_WEB_THEME_OPTIONS_BUFFER_SIZE);
    softap_auth_options_html = calloc(1, ROUTER_WEB_SOFTAP_AUTH_OPTIONS_BUFFER_SIZE);
    clients = calloc(ROUTER_WEB_MAX_CLIENT_ROWS, sizeof(repeater_client_info_t));

    if (options_html == NULL || theme_options_html == NULL || softap_auth_options_html == NULL ||
        clients == NULL) {
        response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        goto cleanup;
    }

    if (html_escape_string(station_config != NULL ? station_config->ssid : "",
                           escaped_station_ssid, sizeof(escaped_station_ssid)) != ESP_OK ||
        html_escape_string(station_config != NULL ? station_config->password : "",
                           escaped_station_password, sizeof(escaped_station_password)) != ESP_OK ||
        html_escape_string(backup_station_config != NULL ? backup_station_config->ssid : "",
                           escaped_backup_station_ssid, sizeof(escaped_backup_station_ssid)) != ESP_OK ||
        html_escape_string(backup_station_config != NULL ? backup_station_config->password : "",
                           escaped_backup_station_password, sizeof(escaped_backup_station_password)) != ESP_OK ||
        html_escape_string(softap_config != NULL ? softap_config->ssid : "",
                           escaped_softap_ssid, sizeof(escaped_softap_ssid)) != ESP_OK ||
        html_escape_string(softap_config != NULL ? softap_config->password : "",
                           escaped_softap_password, sizeof(escaped_softap_password)) != ESP_OK ||
        html_escape_string(web_auth_config != NULL ? web_auth_config->username : "",
                           escaped_web_username, sizeof(escaped_web_username)) != ESP_OK ||
        html_escape_string(web_auth_config != NULL ? web_auth_config->password : "",
                           escaped_web_password, sizeof(escaped_web_password)) != ESP_OK ||
        html_escape_string(active_upstream_ssid != NULL ? active_upstream_ssid : "",
                           escaped_active_upstream_ssid, sizeof(escaped_active_upstream_ssid)) != ESP_OK) {
        response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                           "Settings buffer too small");
        goto cleanup;
    }

    client_count = s_context->get_clients != NULL
        ? s_context->get_clients(clients, ROUTER_WEB_MAX_CLIENT_ROWS)
        : 0;

    clients_html_capacity += client_count * ROUTER_WEB_CLIENT_ROW_ESTIMATE;
    clients_html = calloc(1, clients_html_capacity);
    if (clients_html == NULL) {
        response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        goto cleanup;
    }

    for (size_t i = 0; i < s_context->signal_level_count; ++i) {
        const repeater_signal_level_t *level = &s_context->signal_levels[i];
        if (append_select_option(options_html, ROUTER_WEB_OPTIONS_BUFFER_SIZE, &offset,
                                 level->token, s_context->get_signal_level_token(),
                                 level->label) != ESP_OK) {
            response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                               "Options buffer too small");
            goto cleanup;
        }
    }

    for (size_t i = 0; i < s_context->theme_option_count; ++i) {
        const repeater_theme_option_t *theme = &s_context->theme_options[i];
        if (append_select_option(theme_options_html, ROUTER_WEB_THEME_OPTIONS_BUFFER_SIZE, &theme_offset,
                                 theme->token, s_context->get_theme_token(),
                                 theme->label) != ESP_OK) {
            response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                               "Theme buffer too small");
            goto cleanup;
        }
    }

    for (size_t i = 0; i < s_context->softap_auth_option_count; ++i) {
        const repeater_softap_auth_option_t *option = &s_context->softap_auth_options[i];
        if (append_select_option(softap_auth_options_html, ROUTER_WEB_SOFTAP_AUTH_OPTIONS_BUFFER_SIZE,
                                 &softap_auth_offset, option->token,
                                 softap_auth_token, option->label) != ESP_OK) {
            response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                               "SoftAP auth buffer too small");
            goto cleanup;
        }
    }

    if (client_count == 0) {
        int written = snprintf(
            clients_html, clients_html_capacity,
            "<tr><td colspan=\"5\">No clients connected yet</td></tr>");

        if (written < 0 || written >= (int)clients_html_capacity) {
            response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                               "Clients buffer too small");
            goto cleanup;
        }
    } else {
        for (size_t i = 0; i < client_count; ++i) {
            char escaped_description[ROUTER_WEB_ESCAPED_DEVICE_DESCRIPTION_BUFFER_SIZE];
            char first_seen[20];
            char last_seen[20];
            const char *connection_label = clients[i].is_connected ? "Online" : "Offline";
            const char *connection_class = clients[i].is_connected ? "is-online" : "is-offline";
            const char *signal_html = clients[i].is_connected
                ? (clients[i].rssi >= -60
                    ? "<span class=\"rssi-good\">%d dBm</span>"
                    : clients[i].rssi >= -75
                        ? "<span class=\"rssi-mid\">%d dBm</span>"
                        : "<span class=\"rssi-low\">%d dBm</span>")
                : "<span class=\"meta\">Offline</span>";

            if (html_escape_string(clients[i].description, escaped_description,
                                   sizeof(escaped_description)) != ESP_OK) {
                response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                   "Description buffer too small");
                goto cleanup;
            }

            format_client_timestamp(clients[i].first_seen_epoch, first_seen, sizeof(first_seen));
            format_client_timestamp(clients[i].last_seen_epoch, last_seen, sizeof(last_seen));

            int written = snprintf(
                clients_html + clients_offset,
                clients_html_capacity - clients_offset,
                "<tr>"
                "<td><div class=\"client-id\"><strong>%s</strong><small><span class=\"%s\">%s</span></small></div></td>"
                "<td>",
                clients[i].mac, connection_class, connection_label);

            if (written < 0 || (size_t)written >= clients_html_capacity - clients_offset) {
                response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                   "Clients buffer too small");
                goto cleanup;
            }

            clients_offset += (size_t)written;

            written = snprintf(
                clients_html + clients_offset,
                clients_html_capacity - clients_offset,
                signal_html,
                clients[i].rssi);

            if (written < 0 || (size_t)written >= clients_html_capacity - clients_offset) {
                response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                   "Clients buffer too small");
                goto cleanup;
            }

            clients_offset += (size_t)written;

            written = snprintf(
                clients_html + clients_offset,
                clients_html_capacity - clients_offset,
                "</td>"
                "<td>%s</td>"
                "<td>%s</td>"
                "<td>"
                "<form class=\"inline-label-form\" method=\"post\" action=\"/device-label\">"
                "<input type=\"hidden\" name=\"mac\" value=\"%s\">"
                "<input class=\"text-input\" type=\"text\" name=\"description\" maxlength=\"" ROUTER_WEB_DEVICE_DESCRIPTION_MAX_LEN_STR "\" placeholder=\"Device name or note\" value=\"%s\">"
                "<button class=\"compact-button\" type=\"submit\">Save</button>"
                "</form>"
                "</td>"
                "</tr>",
                first_seen, last_seen, clients[i].mac, escaped_description);

            if (written < 0 || (size_t)written >= clients_html_capacity - clients_offset) {
                response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                   "Clients buffer too small");
                goto cleanup;
            }

            clients_offset += (size_t)written;
        }
    }

    snprintf(reboot_time, sizeof(reboot_time), "%02u:%02u",
             reboot_hour, reboot_minute);
    snprintf(reboot_summary, sizeof(reboot_summary), "%s %02u:%02u",
             auto_reboot_config.enabled ? "On" : "Off",
             reboot_hour, reboot_minute);
    const bool upstream_connected = s_context->is_upstream_connected();
    const char *upstream_status = upstream_connected ? "Online" : "Offline";
    const char *upstream_status_class = upstream_connected ? "is-online" : "is-offline";
    const float chip_temperature_c = s_context->get_chip_temperature_c != NULL
        ? s_context->get_chip_temperature_c()
        : NAN;
    esp_netif_ip_info_t upstream_ip_info;

    snprintf(upstream_ip_text, sizeof(upstream_ip_text), "%s", "Not assigned");
    if (s_context->get_upstream_ip_info != NULL &&
        s_context->get_upstream_ip_info(&upstream_ip_info) == ESP_OK &&
        upstream_ip_info.ip.addr != 0) {
        snprintf(upstream_ip_text, sizeof(upstream_ip_text), IPSTR, IP2STR(&upstream_ip_info.ip));
    }

    if (html_escape_string(upstream_ip_text, escaped_active_upstream_ip,
                           sizeof(escaped_active_upstream_ip)) != ESP_OK) {
        response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                           "IP buffer too small");
        goto cleanup;
    }

    format_temperature_text(chip_temperature_c, chip_temperature, sizeof(chip_temperature),
                            "Unavailable");

    if (s_context->get_firmware_status != NULL) {
        s_context->get_firmware_status(&firmware_status);
    }

    const char *firmware_current_version =
        firmware_status.current_version[0] != '\0' ? firmware_status.current_version : "Unknown";
    const char *firmware_available_version =
        firmware_status.available_version[0] != '\0' ? firmware_status.available_version : "Not checked";
    const char *firmware_status_message =
        firmware_status.status_message[0] != '\0' ? firmware_status.status_message : "No firmware checks yet";
    const char *firmware_state_label =
        !firmware_status.manifest_configured ? "Server not set"
        : firmware_status.update_in_progress ? "Updating"
        : firmware_status.update_available ? "Update available"
        : firmware_status.check_completed ? "Up to date"
        : "Ready";
    const char *firmware_state_class =
        !firmware_status.manifest_configured ? "status-bad"
        : firmware_status.update_in_progress ? "status-warn"
        : firmware_status.update_available ? "status-warn"
        : firmware_status.check_completed ? "status-good"
        : "status-neutral";
    const char *firmware_check_disabled =
        firmware_status.update_in_progress ? "disabled" : "";
    const char *firmware_update_disabled =
        (!firmware_status.manifest_configured ||
         firmware_status.update_in_progress ||
         !firmware_status.update_available) ? "disabled" : "";

    if (html_escape_string(firmware_current_version, escaped_firmware_current_version,
                           sizeof(escaped_firmware_current_version)) != ESP_OK ||
        html_escape_string(firmware_available_version, escaped_firmware_available_version,
                           sizeof(escaped_firmware_available_version)) != ESP_OK ||
        html_escape_string(firmware_status_message, escaped_firmware_status_message,
                           sizeof(escaped_firmware_status_message)) != ESP_OK) {
        response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                           "Firmware buffer too small");
        goto cleanup;
    }

    free(clients);
    clients = NULL;

    html_capacity += clients_offset;
    html = malloc(html_capacity);
    if (html == NULL) {
        response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        goto cleanup;
    }

    int written = snprintf(
        html, html_capacity,
        "<!doctype html>"
        "<html data-theme=\"%s\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>" PROJECT_DEVICE_NAME "</title>"
        "<style>"
        ":root{color-scheme:light;"
        "--bg:#f3f4f6;--panel:#ffffff;--panel-alt:#f8fafc;--text:#1f2937;--muted:#6b7280;--border:#d6dce5;"
        "--accent:#2563eb;--accent-text:#ffffff;--success:#15803d;--danger:#dc2626;--warning:#b45309;--table-row:#fbfcfd;"
        "--font-main:'Segoe UI',Tahoma,sans-serif;--space-1:8px;--space-2:12px;--space-3:16px;--space-4:20px;--space-5:24px;"
        "--radius:12px;--line-copy:1.5;}"
        "html[data-theme='dark']{color-scheme:dark;"
        "--bg:#111827;--panel:#1f2937;--panel-alt:#273244;--text:#f3f4f6;--muted:#c0cad8;--border:#374151;"
        "--accent:#60a5fa;--accent-text:#111827;--success:#4ade80;--danger:#fb7185;--warning:#f59e0b;--table-row:#202a3a;}"
        "*{box-sizing:border-box;font-family:var(--font-main);}"
        "html,body{margin:0;min-height:100vh;}"
        "body{background:var(--bg);color:var(--text);line-height:var(--line-copy);}"
        ".shell{max-width:960px;margin:0 auto;padding:var(--space-4);display:grid;gap:var(--space-3);}"
        ".topbar,.panel,.info-card,.settings-block,table{border:1px solid var(--border);border-radius:var(--radius);background:var(--panel);}"
        ".topbar,.panel{padding:var(--space-4);}"
        ".topbar{display:block;}"
        ".brand{display:block;min-width:0;width:100%%;text-align:left;}"
        ".brand > div{display:grid;gap:var(--space-1);}"
        ".brand p{margin:0;}"
        ".eyebrow{color:var(--muted);font-size:11px;letter-spacing:.08em;text-transform:uppercase;line-height:1.3;}"
        ".brand h1{margin:0;font-size:24px;line-height:1.2;}"
        ".brand p:last-child,.brand-mac{color:var(--muted);font-size:13px;line-height:1.4;}"
        ".is-online{color:var(--success);}"
        ".is-offline{color:var(--danger);}"
        ".content-grid,.stack{display:grid;gap:var(--space-3);}"
        ".stack{display:grid;gap:var(--space-3);}"
        ".panel{display:grid;gap:var(--space-3);}"
        ".notice-banner{gap:6px;border-color:rgba(21,128,61,.25);background:rgba(21,128,61,.08);}"
        ".notice-banner strong{font-size:15px;line-height:1.3;color:var(--success);}"
        ".notice-banner p{margin:0;color:var(--text);font-size:14px;line-height:var(--line-copy);}"
        ".panel-header{display:grid;gap:var(--space-1);margin:0;}"
        ".panel-title{display:grid;gap:var(--space-1);}"
        ".panel-title h3{margin:0;font-size:18px;line-height:1.3;}"
        ".panel-title p{margin:0;color:var(--muted);font-size:13px;line-height:var(--line-copy);}"
        ".meta{display:inline-flex;align-items:center;width:max-content;padding:4px 10px;border-radius:999px;background:var(--panel-alt);"
        "color:var(--muted);font-size:11px;font-weight:700;text-transform:uppercase;}"
        ".info-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:var(--space-2);}"
        ".settings-stack{display:grid;gap:var(--space-2);}"
        ".info-card,.settings-block{display:grid;gap:var(--space-2);padding:var(--space-3);min-height:100%%;}"
        ".info-card strong,.settings-block h4{display:block;margin:0;font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);line-height:1.3;}"
        ".info-card span{display:block;font-size:16px;font-weight:700;line-height:1.3;word-break:break-word;}"
        ".info-card small,.settings-block p,.hint{display:block;margin:0;color:var(--muted);font-size:13px;line-height:var(--line-copy);}"
        ".settings-form{display:grid;gap:var(--space-3);}"
        ".settings-submit{margin:0;}"
        ".button-row{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:var(--space-2);}"
        ".button-form{margin:0;}"
        ".field-label{display:block;margin:0;color:var(--muted);font-size:12px;font-weight:700;line-height:1.3;}"
        "select,input[type='time'],.text-input{width:100%%;padding:12px;font-size:14px;border-radius:var(--radius);border:1px solid var(--border);"
        "background:var(--panel-alt);color:var(--text);margin:0;line-height:1.4;}"
        "button{width:100%%;padding:12px;border:none;border-radius:var(--radius);background:var(--accent);"
        "color:var(--accent-text);font-size:14px;font-weight:700;cursor:pointer;line-height:1.3;}"
        ".button-secondary{background:var(--panel-alt);color:var(--text);border:1px solid var(--border);}"
        "button:hover{filter:brightness(.98);}"
        "button[disabled]{opacity:.55;cursor:not-allowed;filter:none;}"
        ".toggle{display:flex;align-items:center;gap:var(--space-2);padding:12px;margin:0;border:1px solid var(--border);"
        "border-radius:var(--radius);background:var(--panel-alt);color:var(--text);font-weight:600;font-size:14px;line-height:1.35;}"
        ".toggle input{width:16px;height:16px;accent-color:var(--accent);}"
        ".status-good{color:var(--success);}"
        ".status-warn{color:var(--warning);}"
        ".status-bad{color:var(--danger);}"
        ".status-neutral{color:var(--text);}"
        "table{width:100%%;border-collapse:separate;border-spacing:0;overflow:hidden;}"
        "thead th{padding:12px;font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);"
        "text-align:left;background:var(--panel-alt);border-bottom:1px solid var(--border);line-height:1.3;}"
        "tbody td{padding:12px;border-bottom:1px solid var(--border);font-size:14px;vertical-align:top;line-height:var(--line-copy);}"
        "tbody tr:nth-child(odd){background:var(--table-row);}"
        "tbody tr:last-child td{border-bottom:none;}"
        ".client-id strong{display:block;font-size:14px;line-height:1.3;}"
        ".client-id small{display:block;margin-top:4px;color:var(--muted);font-size:12px;line-height:var(--line-copy);}"
        ".inline-label-form{display:grid;grid-template-columns:minmax(0,1fr) 88px;gap:var(--space-2);align-items:start;}"
        ".compact-button{margin:0;padding:12px 10px;font-size:13px;}"
        ".rssi-good{color:var(--success);font-weight:700;}"
        ".rssi-mid{color:var(--warning);font-weight:700;}"
        ".rssi-low{color:var(--danger);font-weight:700;}"
        "@media (max-width:640px){.shell{padding:var(--space-3);}.brand h1{font-size:20px;}.info-grid{grid-template-columns:1fr;}.inline-label-form{grid-template-columns:1fr;}.button-row{grid-template-columns:1fr;}}"
        "</style></head><body>"
        "<div class=\"shell\">"
        "<header class=\"topbar\">"
        "<div class=\"brand\">"
        "<div>"
        "<p class=\"eyebrow\">Router Console</p>"
        "<h1>" PROJECT_DEVICE_NAME "</h1>"
        "<p class=\"brand-mac\">Repeater MAC: %s</p>"
        "<p>Simple repeater settings page.</p>"
        "</div>"
        "</div>"
        "</header>"
        "%s"
        "<section class=\"content-grid\">"
        "<div class=\"stack\">"
        "<div class=\"panel\">"
        "<div class=\"panel-header\"><div class=\"panel-title\"><h3>Overview</h3><p>Key repeater status at a glance.</p></div><span class=\"meta\">Live</span></div>"
        "<div class=\"info-grid\">"
        "<div class=\"info-card\"><strong>Internet</strong><span class=\"%s\">%s</span><small>Upstream IP: %s</small><small>%s uplink active</small></div>"
        "<div class=\"info-card\"><strong>Active Uplink</strong><span>%s</span><small>Main network: %s</small><small>%s profile in use</small></div>"
        "<div class=\"info-card\"><strong>Backup Uplink</strong><span>%s</span><small>%s</small><small>Used only during failover</small></div>"
        "<div class=\"info-card\"><strong>Local Wi-Fi</strong><span>%s</span><small>Open http://192.168.4.1 after connecting</small><small>Clients online: %d</small></div>"
        "<div class=\"info-card\"><strong>Radio</strong><span>%s</span><small>Theme: %s</small><small>Chip temp: %s</small></div>"
        "<div class=\"info-card\"><strong>Maintenance</strong><span>%s</span><small>Repeater MAC: %s</small><small>Web access is protected by login</small></div>"
        "</div>"
        "</div>"
        "<div class=\"panel\">"
        "<div class=\"panel-header\"><div class=\"panel-title\"><h3>Settings</h3><p>All editable repeater settings in one place.</p></div><span class=\"meta\">Config</span></div>"
        "<form class=\"settings-form\" method=\"post\" action=\"/settings\">"
        "<div class=\"settings-stack\">"
        "<div class=\"settings-block\">"
        "<h4>Primary Uplink</h4>"
        "<p>Main upstream Wi-Fi credentials. The repeater tries this network first.</p>"
        "<label class=\"field-label\" for=\"station_ssid\">Primary SSID</label>"
        "<input id=\"station_ssid\" class=\"text-input\" type=\"text\" name=\"station_ssid\" maxlength=\"" ROUTER_WEB_WIFI_SSID_MAX_LEN_STR "\" value=\"%s\" autocomplete=\"off\">"
        "<label class=\"field-label\" for=\"station_password\">Primary password</label>"
        "<input id=\"station_password\" class=\"text-input\" type=\"password\" name=\"station_password\" maxlength=\"" ROUTER_WEB_WIFI_PASSWORD_MAX_LEN_STR "\" value=\"%s\" autocomplete=\"current-password\">"
        "<p class=\"hint\">Leave the password empty only for open upstream networks.</p>"
        "</div>"
        "<div class=\"settings-block\">"
        "<h4>Backup Uplink</h4>"
        "<p>Reserve upstream Wi-Fi credentials used automatically during failover.</p>"
        "<label class=\"field-label\" for=\"backup_station_ssid\">Backup SSID</label>"
        "<input id=\"backup_station_ssid\" class=\"text-input\" type=\"text\" name=\"backup_station_ssid\" maxlength=\"" ROUTER_WEB_WIFI_SSID_MAX_LEN_STR "\" value=\"%s\" autocomplete=\"off\">"
        "<label class=\"field-label\" for=\"backup_station_password\">Backup password</label>"
        "<input id=\"backup_station_password\" class=\"text-input\" type=\"password\" name=\"backup_station_password\" maxlength=\"" ROUTER_WEB_WIFI_PASSWORD_MAX_LEN_STR "\" value=\"%s\" autocomplete=\"current-password\">"
        "<p class=\"hint\">Leave both fields empty to disable the backup uplink.</p>"
        "</div>"
        "<div class=\"settings-block\">"
        "<h4>SoftAP</h4>"
        "<p>Local Wi-Fi credentials broadcast by the repeater.</p>"
        "<label class=\"field-label\" for=\"softap_ssid\">SoftAP SSID</label>"
        "<input id=\"softap_ssid\" class=\"text-input\" type=\"text\" name=\"softap_ssid\" maxlength=\"" ROUTER_WEB_WIFI_SSID_MAX_LEN_STR "\" value=\"%s\" autocomplete=\"off\">"
        "<label class=\"field-label\" for=\"softap_password\">SoftAP password</label>"
        "<input id=\"softap_password\" class=\"text-input\" type=\"password\" name=\"softap_password\" maxlength=\"" ROUTER_WEB_WIFI_PASSWORD_MAX_LEN_STR "\" value=\"%s\" autocomplete=\"current-password\">"
        "<label class=\"field-label\" for=\"softap_auth\">SoftAP encryption</label>"
        "<select id=\"softap_auth\" name=\"softap_auth\">%s</select>"
        "<p class=\"hint\">Leave the password empty to make the local network open.</p>"
        "</div>"
        "<div class=\"settings-block\">"
        "<h4>Web Access</h4>"
        "<p>Credentials for the local settings page.</p>"
        "<label class=\"field-label\" for=\"web_username\">Web login</label>"
        "<input id=\"web_username\" class=\"text-input\" type=\"text\" name=\"web_username\" maxlength=\"" ROUTER_WEB_AUTH_USERNAME_MAX_LEN_STR "\" value=\"%s\" autocomplete=\"username\">"
        "<label class=\"field-label\" for=\"web_password\">Web password</label>"
        "<input id=\"web_password\" class=\"text-input\" type=\"password\" name=\"web_password\" maxlength=\"" ROUTER_WEB_AUTH_PASSWORD_MAX_LEN_STR "\" value=\"%s\" autocomplete=\"current-password\">"
        "<p class=\"hint\">Applied immediately after saving. Use the new credentials on the next request.</p>"
        "</div>"
        "<div class=\"settings-block\">"
        "<h4>Radio</h4>"
        "<p>Transmit power preset.</p>"
        "<label class=\"field-label\" for=\"level\">Signal level</label>"
        "<select id=\"level\" name=\"level\">%s</select>"
        "</div>"
        "<div class=\"settings-block\">"
        "<h4>Appearance</h4>"
        "<p>Light or dark theme.</p>"
        "<label class=\"field-label\" for=\"theme\">Interface theme</label>"
        "<select id=\"theme\" name=\"theme\">%s</select>"
        "</div>"
        "<div class=\"settings-block\">"
        "<h4>Protection</h4>"
        "<p>Scheduled restart.</p>"
        "<label class=\"toggle\" for=\"reboot_enabled\">"
        "<input id=\"reboot_enabled\" type=\"checkbox\" name=\"reboot_enabled\" value=\"1\" %s>"
        "<span>Enable scheduled reboot</span>"
        "</label>"
        "<label class=\"field-label\" for=\"reboot_time\">Daily reboot time</label>"
        "<input id=\"reboot_time\" type=\"time\" name=\"reboot_time\" step=\"60\" value=\"%s\">"
        "<p class=\"hint\">Runs after time sync from NTP.</p>"
        "</div>"
        "</div>"
        "<button class=\"settings-submit\" type=\"submit\">Save all</button>"
        "</form>"
        "</div>"
        "</div>"
        "<div class=\"panel\">"
        "<div class=\"panel-header\"><div class=\"panel-title\"><h3>Connected Clients</h3><p>All discovered clients in first-connection order. Leave description empty to remove a label.</p></div><span class=\"meta\">History</span></div>"
        "<table>"
        "<thead><tr><th>MAC address</th><th>Signal level</th><th>First seen</th><th>Last seen</th><th>Device description</th></tr></thead>"
        "<tbody>%s</tbody>"
        "</table>"
        "</div>"
        "<div class=\"panel\">"
        "<div class=\"panel-header\"><div class=\"panel-title\"><h3>Firmware Update</h3><p>Check the server version and install a newer build when available.</p></div><span class=\"meta\">OTA</span></div>"
        "<div class=\"info-grid\">"
        "<div class=\"info-card\"><strong>Installed</strong><span>%s</span><small>Current running firmware</small></div>"
        "<div class=\"info-card\"><strong>Latest</strong><span>%s</span><small>Newest version reported by the server</small></div>"
        "<div class=\"info-card\"><strong>Status</strong><span class=\"%s\">%s</span><small>%s</small></div>"
        "</div>"
        "<div class=\"button-row\">"
        "<form class=\"button-form\" method=\"post\" action=\"/firmware/check\">"
        "<button class=\"button-secondary\" type=\"submit\" %s>Check</button>"
        "</form>"
        "<form class=\"button-form\" method=\"post\" action=\"/firmware/update\">"
        "<button type=\"submit\" %s>Update</button>"
        "</form>"
        "</div>"
        "</div>"
        "</section>"
        "</body></html>",
        s_context->get_theme_token(),
        s_context->esp_mac != NULL ? s_context->esp_mac : "Unknown",
        notice_html,
        upstream_status_class,
        upstream_status,
        escaped_active_upstream_ip,
        active_upstream_role,
        escaped_active_upstream_ssid,
        escaped_station_ssid,
        active_upstream_role,
        escaped_backup_station_ssid[0] != '\0' ? escaped_backup_station_ssid : "Disabled",
        escaped_backup_station_ssid[0] != '\0'
            ? "Configured as reserve uplink"
            : "No backup uplink configured",
        escaped_softap_ssid,
        s_context->get_client_count(),
        s_context->get_signal_level_label(),
        s_context->get_theme_label(),
        chip_temperature,
        reboot_summary,
        s_context->esp_mac != NULL ? s_context->esp_mac : "Unknown",
        escaped_station_ssid,
        escaped_station_password,
        escaped_backup_station_ssid,
        escaped_backup_station_password,
        escaped_softap_ssid,
        escaped_softap_password,
        softap_auth_options_html,
        escaped_web_username,
        escaped_web_password,
        options_html,
        theme_options_html,
        auto_reboot_config.enabled ? "checked" : "",
        reboot_time,
        clients_html,
        escaped_firmware_current_version,
        escaped_firmware_available_version,
        firmware_state_class,
        firmware_state_label,
        escaped_firmware_status_message,
        firmware_check_disabled,
        firmware_update_disabled);

    if (written < 0 || written >= (int)html_capacity) {
        response_err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                           "HTML buffer too small");
        goto cleanup;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    response_err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);

cleanup:
    free(options_html);
    free(theme_options_html);
    free(softap_auth_options_html);
    free(clients_html);
    free(clients);
    free(html);
    return response_err;
}

static esp_err_t redirect_to_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t redirect_to_location(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location != NULL ? location : "/");
    return httpd_resp_send(req, NULL, 0);
}

static bool strings_equal_or_empty(const char *left, const char *right)
{
    const char *lhs = left != NULL ? left : "";
    const char *rhs = right != NULL ? right : "";
    return strcmp(lhs, rhs) == 0;
}

static esp_err_t send_settings_saved_page(httpd_req_t *req, bool softap_changed,
                                          bool web_auth_changed, const char *softap_ssid)
{
    char escaped_softap_ssid[ROUTER_WEB_ESCAPED_SSID_BUFFER_SIZE];
    char html[3072];
    const char *safe_softap_ssid = softap_ssid != NULL ? softap_ssid : "";

    if (html_escape_string(safe_softap_ssid, escaped_softap_ssid,
                           sizeof(escaped_softap_ssid)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "SoftAP SSID buffer too small");
    }

    int written = 0;

    if (softap_changed) {
        written = snprintf(
            html, sizeof(html),
            "<!doctype html>"
            "<html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Settings Saved</title>"
            "<style>"
            "body{margin:0;font-family:'Segoe UI',Tahoma,sans-serif;background:#f3f4f6;color:#1f2937;display:flex;min-height:100vh;align-items:center;justify-content:center;padding:20px;line-height:1.5;}"
            ".card{max-width:560px;width:100%%;background:#fff;border:1px solid #d6dce5;border-radius:12px;padding:24px;}"
            "h1{margin:0 0 12px;font-size:24px;line-height:1.2;}p{margin:0 0 12px;color:#6b7280;font-size:14px;}"
            ".note{padding:16px;border:1px solid #d6dce5;border-radius:12px;background:#f8fafc;margin:16px 0;}"
            ".action{display:inline-block;margin-top:4px;padding:12px 16px;border-radius:12px;background:#2563eb;color:#fff;text-decoration:none;font-size:14px;font-weight:700;}"
            "</style></head><body><main class=\"card\">"
            "<h1>Settings saved</h1>"
            "<p>The repeater is applying new Wi-Fi settings. The current page may disconnect in a moment.</p>"
            "<div class=\"note\"><p>Reconnect to the updated repeater Wi-Fi and then open the home page again.</p>"
            "<p><strong>New SoftAP SSID:</strong> %s</p>"
            "%s"
            "</div>"
            "<a class=\"action\" href=\"/\">Open home page</a>"
            "</main></body></html>",
            escaped_softap_ssid,
            web_auth_changed ? "<p>The next page load will also use the updated web login.</p>" : "");

        if (written < 0 || written >= (int)sizeof(html)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Success page buffer too small");
        }
    } else if (web_auth_changed) {
        written = snprintf(
            html, sizeof(html),
            "<!doctype html>"
            "<html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Settings Saved</title>"
            "<style>"
            "body{margin:0;font-family:'Segoe UI',Tahoma,sans-serif;background:#f3f4f6;color:#1f2937;display:flex;min-height:100vh;align-items:center;justify-content:center;padding:20px;line-height:1.5;}"
            ".card{max-width:560px;width:100%%;background:#fff;border:1px solid #d6dce5;border-radius:12px;padding:24px;}"
            "h1{margin:0 0 12px;font-size:24px;line-height:1.2;}p{margin:0 0 12px;color:#6b7280;font-size:14px;}"
            ".note{padding:16px;border:1px solid #d6dce5;border-radius:12px;background:#f8fafc;margin:16px 0;}"
            ".action{display:inline-block;margin-top:4px;padding:12px 16px;border-radius:12px;background:#2563eb;color:#fff;text-decoration:none;font-size:14px;font-weight:700;}"
            "</style></head><body><main class=\"card\">"
            "<h1>Settings saved</h1>"
            "<p>The new settings have been saved successfully.</p>"
            "<div class=\"note\"><p>Use the updated web login on the next page load.</p></div>"
            "<a class=\"action\" href=\"/\">Open home page</a>"
            "</main></body></html>");

        if (written < 0 || written >= (int)sizeof(html)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Success page buffer too small");
        }
    } else {
        return redirect_to_location(req, "/?notice=settings-saved");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_firmware_action_page(httpd_req_t *req, const char *title,
                                           const char *summary,
                                           const repeater_firmware_status_t *status)
{
    char escaped_current_version[ROUTER_WEB_ESCAPED_FIRMWARE_VERSION_BUFFER_SIZE];
    char escaped_available_version[ROUTER_WEB_ESCAPED_FIRMWARE_VERSION_BUFFER_SIZE];
    char escaped_status_message[ROUTER_WEB_ESCAPED_FIRMWARE_MESSAGE_BUFFER_SIZE];
    char html[4096];
    const char *safe_title = title != NULL ? title : "Firmware Update";
    const char *safe_summary = summary != NULL ? summary : "";
    const char *safe_current_version =
        status != NULL && status->current_version[0] != '\0' ? status->current_version : "Unknown";
    const char *safe_available_version =
        status != NULL && status->available_version[0] != '\0' ? status->available_version : "Unknown";
    const char *safe_status_message =
        status != NULL && status->status_message[0] != '\0' ? status->status_message : "No details";
    const char *status_badge =
        status != NULL && status->update_in_progress ? "In progress"
        : status != NULL && status->update_available ? "Update available"
        : status != NULL && status->last_action_succeeded ? "Ready"
        : "Attention";

    if (html_escape_string(safe_current_version, escaped_current_version,
                           sizeof(escaped_current_version)) != ESP_OK ||
        html_escape_string(safe_available_version, escaped_available_version,
                           sizeof(escaped_available_version)) != ESP_OK ||
        html_escape_string(safe_status_message, escaped_status_message,
                           sizeof(escaped_status_message)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Firmware page buffer too small");
    }

    int written = snprintf(
        html, sizeof(html),
        "<!doctype html>"
        "<html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>%s</title>"
        "<style>"
        "body{margin:0;font-family:'Segoe UI',Tahoma,sans-serif;background:#f3f4f6;color:#1f2937;display:flex;min-height:100vh;align-items:center;justify-content:center;padding:20px;line-height:1.5;}"
        ".card{max-width:640px;width:100%%;background:#fff;border:1px solid #d6dce5;border-radius:12px;padding:24px;display:grid;gap:16px;}"
        "h1{margin:0;font-size:24px;line-height:1.2;}p{margin:0;color:#6b7280;font-size:14px;}"
        ".badge{display:inline-flex;align-items:center;width:max-content;padding:4px 10px;border-radius:999px;background:#eff6ff;color:#2563eb;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.06em;}"
        ".note{padding:16px;border:1px solid #d6dce5;border-radius:12px;background:#f8fafc;display:grid;gap:8px;}"
        ".actions{display:flex;flex-wrap:wrap;gap:12px;}"
        ".action{display:inline-block;padding:12px 16px;border-radius:12px;background:#2563eb;color:#fff;text-decoration:none;font-size:14px;font-weight:700;}"
        ".action.secondary{background:#e5e7eb;color:#1f2937;}"
        "</style></head><body><main class=\"card\">"
        "<span class=\"badge\">%s</span>"
        "<h1>%s</h1>"
        "<p>%s</p>"
        "<div class=\"note\">"
        "<p><strong>Installed version:</strong> %s</p>"
        "<p><strong>Latest version:</strong> %s</p>"
        "<p><strong>Status:</strong> %s</p>"
        "</div>"
        "<div class=\"actions\">"
        "<a class=\"action\" href=\"/\">Back to panel</a>"
        "<a class=\"action secondary\" href=\"/\">Refresh status</a>"
        "</div>"
        "</main></body></html>",
        safe_title, status_badge, safe_title, safe_summary, escaped_current_version,
        escaped_available_version, escaped_status_message);

    if (written < 0 || written >= (int)sizeof(html)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Firmware page buffer too small");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t web_signal_post_handler(httpd_req_t *req)
{
    char body[128];
    char level[16];

    if (!is_request_authorized(req)) {
        ESP_LOGW(TAG, "Unauthorized signal change request");
        return send_auth_challenge(req);
    }

    esp_err_t err = read_form_body(req, body, sizeof(body));
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
    }

    err = read_form_value(body, "level", level, sizeof(level));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing signal level");
    }

    ESP_RETURN_ON_ERROR(s_context->set_signal_level_by_token(level), TAG,
                        "Failed to change signal level from web UI");

    ESP_LOGI(TAG, "Signal level changed from web UI to: %s", level);
    return redirect_to_root(req);
}

static esp_err_t web_theme_post_handler(httpd_req_t *req)
{
    char body[128];
    char theme[16];

    if (!is_request_authorized(req)) {
        ESP_LOGW(TAG, "Unauthorized theme change request");
        return send_auth_challenge(req);
    }

    esp_err_t err = read_form_body(req, body, sizeof(body));
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
    }

    err = read_form_value(body, "theme", theme, sizeof(theme));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing theme");
    }

    ESP_RETURN_ON_ERROR(s_context->set_theme_by_token(theme), TAG,
                        "Failed to change theme from web UI");

    ESP_LOGI(TAG, "Web theme changed from web UI to: %s", theme);
    return redirect_to_root(req);
}

static esp_err_t web_device_label_post_handler(httpd_req_t *req)
{
    char body[ROUTER_WEB_DEVICE_FORM_BODY_SIZE];
    char mac[REPEATER_MAC_STRING_LEN];
    char description[REPEATER_DEVICE_DESCRIPTION_MAX_LEN + 1];

    if (!is_request_authorized(req)) {
        ESP_LOGW(TAG, "Unauthorized device label change request");
        return send_auth_challenge(req);
    }

    esp_err_t err = read_form_body(req, body, sizeof(body));
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
    }

    err = read_form_value(body, "mac", mac, sizeof(mac));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing MAC address");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid MAC address");
    }

    err = read_form_value(body, "description", description, sizeof(description));
    if (err == ESP_ERR_NOT_FOUND) {
        description[0] = '\0';
    } else if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device description");
    }

    if (s_context->set_device_description == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Device description storage is unavailable");
    }

    ESP_RETURN_ON_ERROR(s_context->set_device_description(mac, description), TAG,
                        "Failed to change device description from web UI");

    ESP_LOGI(TAG, "Device description updated from web UI for MAC %s", mac);
    return redirect_to_location(req, description[0] != '\0'
        ? "/?notice=device-saved"
        : "/?notice=device-removed");
}

static esp_err_t web_auto_reboot_post_handler(httpd_req_t *req)
{
    char body[192];
    char reboot_time[16];
    uint8_t hour = 0;
    uint8_t minute = 0;
    bool enabled = false;

    if (!is_request_authorized(req)) {
        ESP_LOGW(TAG, "Unauthorized auto reboot change request");
        return send_auth_challenge(req);
    }

    esp_err_t err = read_form_body(req, body, sizeof(body));
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
    }

    enabled = httpd_query_key_value(body, "reboot_enabled", reboot_time, sizeof(reboot_time)) == ESP_OK;

    err = read_form_value(body, "reboot_time", reboot_time, sizeof(reboot_time));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing reboot time");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid reboot time");
    }

    if (parse_hhmm_time(reboot_time, &hour, &minute) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Reboot time must be HH:MM");
    }

    ESP_RETURN_ON_ERROR(s_context->set_auto_reboot_config(enabled, hour, minute), TAG,
                        "Failed to change auto reboot config from web UI");

    ESP_LOGI(TAG, "Auto reboot config changed from web UI: %s at %02u:%02u",
             enabled ? "enabled" : "disabled", hour, minute);
    return redirect_to_root(req);
}

static esp_err_t web_firmware_check_post_handler(httpd_req_t *req)
{
    repeater_firmware_status_t firmware_status = { 0 };
    esp_err_t err;

    if (!is_request_authorized(req)) {
        ESP_LOGW(TAG, "Unauthorized firmware check request");
        return send_auth_challenge(req);
    }

    if (s_context->check_firmware_update == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Firmware update checks are unavailable");
    }

    err = s_context->check_firmware_update(&firmware_status);
    if (err != ESP_OK) {
        return send_firmware_action_page(req, "Firmware Check",
                                         "The repeater could not complete the firmware check.",
                                         &firmware_status);
    }

    return send_firmware_action_page(
        req,
        "Firmware Check",
        firmware_status.update_available
            ? "A newer firmware build was found on the server."
            : "This repeater is already running the latest firmware.",
        &firmware_status);
}

static esp_err_t web_firmware_update_post_handler(httpd_req_t *req)
{
    repeater_firmware_status_t firmware_status = { 0 };
    esp_err_t err;

    if (!is_request_authorized(req)) {
        ESP_LOGW(TAG, "Unauthorized firmware update request");
        return send_auth_challenge(req);
    }

    if (s_context->start_firmware_update == NULL || s_context->get_firmware_status == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Firmware updates are unavailable");
    }

    err = s_context->start_firmware_update();
    s_context->get_firmware_status(&firmware_status);

    if (err != ESP_OK) {
        return send_firmware_action_page(req, "Firmware Update",
                                         "The firmware update could not be started.",
                                         &firmware_status);
    }

    return send_firmware_action_page(
        req,
        "Firmware Update",
        "The update task has started. If installation succeeds, the repeater will reboot automatically.",
        &firmware_status);
}

static esp_err_t web_settings_post_handler(httpd_req_t *req)
{
    char body[ROUTER_WEB_SETTINGS_FORM_BODY_SIZE];
    char station_ssid[REPEATER_WIFI_SSID_MAX_LEN + 1];
    char station_password[REPEATER_WIFI_PASSWORD_MAX_LEN + 1];
    char backup_station_ssid[REPEATER_WIFI_SSID_MAX_LEN + 1];
    char backup_station_password[REPEATER_WIFI_PASSWORD_MAX_LEN + 1];
    char softap_ssid[REPEATER_WIFI_SSID_MAX_LEN + 1];
    char softap_password[REPEATER_WIFI_PASSWORD_MAX_LEN + 1];
    char softap_auth[16];
    char web_username[REPEATER_WEB_AUTH_USERNAME_MAX_LEN + 1];
    char web_password[REPEATER_WEB_AUTH_PASSWORD_MAX_LEN + 1];
    char level[16];
    char theme[16];
    char reboot_time[16];
    uint8_t hour = 0;
    uint8_t minute = 0;
    bool enabled = false;
    bool wifi_settings_changed = false;
    bool softap_settings_changed = false;
    bool web_auth_changed = false;
    const repeater_station_config_t *current_station_config =
        s_context->get_station_config != NULL ? s_context->get_station_config() : NULL;
    const repeater_station_config_t *current_backup_station_config =
        s_context->get_backup_station_config != NULL ? s_context->get_backup_station_config() : NULL;
    const repeater_softap_config_t *current_softap_config =
        s_context->get_softap_config != NULL ? s_context->get_softap_config() : NULL;
    const repeater_web_auth_config_t *current_web_auth_config =
        s_context->get_web_auth_config != NULL ? s_context->get_web_auth_config() : NULL;

    if (!is_request_authorized(req)) {
        ESP_LOGW(TAG, "Unauthorized settings change request");
        return send_auth_challenge(req);
    }

    esp_err_t err = read_form_body(req, body, sizeof(body));
    if (err == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
    }

    err = read_form_value(body, "station_ssid", station_ssid, sizeof(station_ssid));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Station SSID");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Station SSID");
    }

    err = read_form_value(body, "station_password", station_password, sizeof(station_password));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Station password");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Station password");
    }

    err = read_form_value(body, "backup_station_ssid", backup_station_ssid,
                          sizeof(backup_station_ssid));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing backup Station SSID");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid backup Station SSID");
    }

    err = read_form_value(body, "backup_station_password", backup_station_password,
                          sizeof(backup_station_password));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing backup Station password");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid backup Station password");
    }

    err = read_form_value(body, "softap_ssid", softap_ssid, sizeof(softap_ssid));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SoftAP SSID");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SoftAP SSID");
    }

    err = read_form_value(body, "softap_password", softap_password, sizeof(softap_password));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SoftAP password");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SoftAP password");
    }

    err = read_form_value(body, "softap_auth", softap_auth, sizeof(softap_auth));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SoftAP encryption type");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SoftAP encryption type");
    }

    err = read_form_value(body, "web_username", web_username, sizeof(web_username));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing web login");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid web login");
    }

    err = read_form_value(body, "web_password", web_password, sizeof(web_password));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing web password");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid web password");
    }

    err = read_form_value(body, "level", level, sizeof(level));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing signal level");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid signal level");
    }

    err = read_form_value(body, "theme", theme, sizeof(theme));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing theme");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid theme");
    }

    enabled = httpd_query_key_value(body, "reboot_enabled", reboot_time, sizeof(reboot_time)) == ESP_OK;

    err = read_form_value(body, "reboot_time", reboot_time, sizeof(reboot_time));
    if (err == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing reboot time");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid reboot time");
    }

    if (parse_hhmm_time(reboot_time, &hour, &minute) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Reboot time must be HH:MM");
    }

    if (s_context->set_wifi_networks == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Wi-Fi settings are unavailable");
    }

    wifi_settings_changed =
        current_station_config == NULL ||
        current_backup_station_config == NULL ||
        current_softap_config == NULL ||
        !strings_equal_or_empty(station_ssid, current_station_config->ssid) ||
        !strings_equal_or_empty(station_password, current_station_config->password) ||
        !strings_equal_or_empty(backup_station_ssid, current_backup_station_config->ssid) ||
        !strings_equal_or_empty(backup_station_password, current_backup_station_config->password) ||
        !strings_equal_or_empty(softap_ssid, current_softap_config->ssid) ||
        !strings_equal_or_empty(softap_password, current_softap_config->password) ||
        !strings_equal_or_empty(softap_auth, s_context->get_softap_auth_token());

    softap_settings_changed =
        current_softap_config == NULL ||
        !strings_equal_or_empty(softap_ssid, current_softap_config->ssid) ||
        !strings_equal_or_empty(softap_password, current_softap_config->password) ||
        !strings_equal_or_empty(softap_auth, s_context->get_softap_auth_token());

    web_auth_changed =
        current_web_auth_config == NULL ||
        !strings_equal_or_empty(web_username, current_web_auth_config->username) ||
        !strings_equal_or_empty(web_password, current_web_auth_config->password);

    if (wifi_settings_changed) {
        err = s_context->set_wifi_networks(station_ssid, station_password,
                                           backup_station_ssid, backup_station_password,
                                           softap_ssid, softap_password, softap_auth);
        if (err == ESP_ERR_INVALID_ARG) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Invalid primary, backup, or SoftAP parameters");
        }
        ESP_RETURN_ON_ERROR(err, TAG,
                            "Failed to change uplink and SoftAP parameters from unified settings form");
    }
    ESP_RETURN_ON_ERROR(s_context->set_signal_level_by_token(level), TAG,
                        "Failed to change signal level from unified settings form");
    ESP_RETURN_ON_ERROR(s_context->set_theme_by_token(theme), TAG,
                        "Failed to change theme from unified settings form");
    ESP_RETURN_ON_ERROR(s_context->set_auto_reboot_config(enabled, hour, minute), TAG,
                        "Failed to change auto reboot config from unified settings form");

    if (s_context->set_web_auth == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Web auth settings are unavailable");
    }

    err = s_context->set_web_auth(web_username, web_password);
    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Invalid web login or password");
    }
    ESP_RETURN_ON_ERROR(err, TAG,
                        "Failed to change web auth parameters from unified settings form");
    if (web_auth_changed) {
        ESP_RETURN_ON_ERROR(build_expected_auth_header(), TAG,
                            "Failed to refresh Basic Auth header after web auth change");
    }

    ESP_LOGI(TAG,
             "Settings updated from web UI: primary=%s backup=%s softap=%s web_user=%s level=%s theme=%s reboot=%s %02u:%02u",
             station_ssid,
             backup_station_ssid[0] != '\0' ? backup_station_ssid : "(disabled)",
             softap_ssid, web_username, level, theme,
             enabled ? "enabled" : "disabled", hour, minute);

    return send_settings_saved_page(req, softap_settings_changed, web_auth_changed,
                                    softap_ssid);
}

esp_err_t router_web_start(const router_web_context_t *context, httpd_handle_t *out_server)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_RETURN_ON_FALSE(context != NULL, ESP_ERR_INVALID_ARG, TAG, "Context is required");
    ESP_RETURN_ON_FALSE(context->is_upstream_connected != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Upstream status callback is required");
    ESP_RETURN_ON_FALSE(context->get_client_count != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Client count callback is required");
    ESP_RETURN_ON_FALSE(context->get_signal_level_label != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Signal level label callback is required");
    ESP_RETURN_ON_FALSE(context->get_signal_level_token != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Signal level token callback is required");
    ESP_RETURN_ON_FALSE(context->set_signal_level_by_token != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Signal level setter callback is required");
    ESP_RETURN_ON_FALSE(context->get_theme_label != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Theme label callback is required");
    ESP_RETURN_ON_FALSE(context->get_theme_token != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Theme token callback is required");
    ESP_RETURN_ON_FALSE(context->set_theme_by_token != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Theme setter callback is required");
    ESP_RETURN_ON_FALSE(context->get_softap_auth_token != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "SoftAP auth token callback is required");
    ESP_RETURN_ON_FALSE(context->get_auto_reboot_config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Auto reboot getter callback is required");
    ESP_RETURN_ON_FALSE(context->set_auto_reboot_config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Auto reboot setter callback is required");
    ESP_RETURN_ON_FALSE(context->get_firmware_status != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Firmware status callback is required");
    ESP_RETURN_ON_FALSE(context->check_firmware_update != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Firmware check callback is required");
    ESP_RETURN_ON_FALSE(context->start_firmware_update != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Firmware update callback is required");
    ESP_RETURN_ON_FALSE(context->signal_levels != NULL || context->signal_level_count == 0,
                        ESP_ERR_INVALID_ARG, TAG, "Signal level options are required");
    ESP_RETURN_ON_FALSE(context->theme_options != NULL || context->theme_option_count == 0,
                        ESP_ERR_INVALID_ARG, TAG, "Theme options are required");
    ESP_RETURN_ON_FALSE(context->softap_auth_options != NULL ||
                        context->softap_auth_option_count == 0,
                        ESP_ERR_INVALID_ARG, TAG, "SoftAP auth options are required");
    s_context_storage = *context;
    ESP_RETURN_ON_ERROR(build_expected_auth_header(), TAG, "Failed to prepare Basic Auth header");
    config.stack_size = ROUTER_WEB_HTTP_STACK_SIZE;
    config.max_uri_handlers = 12;

    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "Failed to start HTTP server");

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t signal = {
        .uri = "/signal",
        .method = HTTP_POST,
        .handler = web_signal_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t theme = {
        .uri = "/theme",
        .method = HTTP_POST,
        .handler = web_theme_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t device_label = {
        .uri = "/device-label",
        .method = HTTP_POST,
        .handler = web_device_label_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t auto_reboot = {
        .uri = "/auto-reboot",
        .method = HTTP_POST,
        .handler = web_auto_reboot_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t settings = {
        .uri = "/settings",
        .method = HTTP_POST,
        .handler = web_settings_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t firmware_check = {
        .uri = "/firmware/check",
        .method = HTTP_POST,
        .handler = web_firmware_check_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t firmware_update = {
        .uri = "/firmware/update",
        .method = HTTP_POST,
        .handler = web_firmware_update_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = web_favicon_get_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG,
                        "Failed to register root handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &signal), TAG,
                        "Failed to register signal handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &theme), TAG,
                        "Failed to register theme handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &device_label), TAG,
                        "Failed to register device label handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &auto_reboot), TAG,
                        "Failed to register auto reboot handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings), TAG,
                        "Failed to register settings handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &firmware_check), TAG,
                        "Failed to register firmware check handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &firmware_update), TAG,
                        "Failed to register firmware update handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &favicon), TAG,
                        "Failed to register favicon handler");

    if (out_server != NULL) {
        *out_server = server;
    }

    const repeater_web_auth_config_t *web_auth_config =
        s_context->get_web_auth_config != NULL ? s_context->get_web_auth_config() : NULL;
    ESP_LOGI(TAG, "Web UI started for %s. Open http://192.168.4.1/ with user '%s'",
             PROJECT_DEVICE_NAME,
             web_auth_config != NULL ? web_auth_config->username : PROJECT_WEB_AUTH_USERNAME);
    return ESP_OK;
}

void router_web_stop(httpd_handle_t server)
{
    if (server != NULL) {
        httpd_stop(server);
    }
}
