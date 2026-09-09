#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "repeater_dhcp_hostnames.h"
#include "repeater_settings.h"
#include "dhcpserver/dhcpserver.h"

typedef struct {
    bool occupied;
    char mac[REPEATER_MAC_STRING_LEN];
    char hostname[REPEATER_CLIENT_HOSTNAME_MAX_LEN + 1];
} repeater_pending_client_hostname_t;

static repeater_pending_client_hostname_t s_pending_client_hostnames[REPEATER_CLIENT_HISTORY_MAX_ENTRIES];
static portMUX_TYPE s_pending_client_hostnames_lock = portMUX_INITIALIZER_UNLOCKED;

#define REPEATER_DHCP_MAGIC_COOKIE_LEN 4
#define REPEATER_DHCP_OPTION_PAD 0
#define REPEATER_DHCP_OPTION_END 255
#define REPEATER_DHCP_OPTION_HOST_NAME 12
#define REPEATER_DHCP_OPTION_FQDN 81

static void repeater_format_client_mac(char *output, size_t output_size, const uint8_t mac[6])
{
    if (output == NULL || mac == NULL || output_size < REPEATER_MAC_STRING_LEN) {
        return;
    }

    snprintf(output, output_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void repeater_trim_hostname_text(char *text)
{
    size_t length;
    size_t start = 0;

    if (text == NULL) {
        return;
    }

    length = strlen(text);
    while (start < length && isspace((unsigned char)text[start])) {
        ++start;
    }
    while (length > start &&
           (isspace((unsigned char)text[length - 1]) || text[length - 1] == '.')) {
        --length;
    }

    if (start > 0 && length > start) {
        memmove(text, text + start, length - start);
    }

    if (length <= start) {
        text[0] = '\0';
        return;
    }

    text[length - start] = '\0';
}

static void repeater_sanitize_hostname_text(const uint8_t *input, size_t input_len,
                                            char *output, size_t output_size)
{
    size_t written = 0;

    if (output == NULL || output_size == 0) {
        return;
    }

    output[0] = '\0';
    if (input == NULL) {
        return;
    }

    for (size_t i = 0; i < input_len && written + 1 < output_size; ++i) {
        const uint8_t ch = input[i];

        if (ch == '\0') {
            break;
        }
        if (ch < 32 || ch > 126) {
            continue;
        }

        output[written++] = (char)ch;
    }

    output[written] = '\0';
    repeater_trim_hostname_text(output);
}

static bool repeater_decode_fqdn_labels(const uint8_t *input, size_t input_len,
                                        char *output, size_t output_size)
{
    size_t offset = 0;
    size_t written = 0;

    if (output == NULL || output_size == 0) {
        return false;
    }

    output[0] = '\0';
    if (input == NULL || input_len == 0) {
        return false;
    }

    while (offset < input_len) {
        const uint8_t label_len = input[offset++];

        if (label_len == 0) {
            break;
        }
        if ((label_len & 0xC0U) != 0 || label_len > input_len - offset) {
            return false;
        }

        if (written != 0) {
            if (written + 1 >= output_size) {
                break;
            }
            output[written++] = '.';
        }

        for (uint8_t i = 0; i < label_len && written + 1 < output_size; ++i) {
            const uint8_t ch = input[offset++];
            if (ch < 32 || ch > 126) {
                return false;
            }
            output[written++] = (char)ch;
        }
    }

    output[written] = '\0';
    repeater_trim_hostname_text(output);
    return output[0] != '\0';
}

static bool repeater_parse_dhcp_hostname(const struct dhcps_msg *msg, u16_t packet_len,
                                         char *out_hostname, size_t out_hostname_size)
{
    static const uint8_t magic_cookie[REPEATER_DHCP_MAGIC_COOKIE_LEN] = { 99, 130, 83, 99 };
    char fqdn_hostname[REPEATER_CLIENT_HOSTNAME_MAX_LEN + 1];
    const size_t options_offset = offsetof(struct dhcps_msg, options);
    const size_t max_options_len = sizeof(((struct dhcps_msg *)0)->options);
    size_t options_len = max_options_len;
    size_t cursor = REPEATER_DHCP_MAGIC_COOKIE_LEN;

    if (out_hostname == NULL || out_hostname_size == 0) {
        return false;
    }

    out_hostname[0] = '\0';
    fqdn_hostname[0] = '\0';

    if (msg == NULL) {
        return false;
    }
    if ((size_t)packet_len > options_offset && (size_t)packet_len - options_offset < max_options_len) {
        options_len = (size_t)packet_len - options_offset;
    }
    if (options_len < REPEATER_DHCP_MAGIC_COOKIE_LEN) {
        return false;
    }
    if (memcmp(msg->options, magic_cookie, sizeof(magic_cookie)) != 0) {
        return false;
    }

    while (cursor < options_len) {
        const uint8_t option = msg->options[cursor++];
        size_t option_len;
        const uint8_t *option_value;

        if (option == REPEATER_DHCP_OPTION_PAD) {
            continue;
        }
        if (option == REPEATER_DHCP_OPTION_END) {
            break;
        }
        if (cursor >= options_len) {
            break;
        }

        option_len = msg->options[cursor++];
        if (option_len > options_len - cursor) {
            break;
        }

        option_value = &msg->options[cursor];
        if (option == REPEATER_DHCP_OPTION_HOST_NAME) {
            repeater_sanitize_hostname_text(option_value, option_len,
                                            out_hostname, out_hostname_size);
            if (out_hostname[0] != '\0') {
                return true;
            }
        } else if (option == REPEATER_DHCP_OPTION_FQDN && option_len > 3 && fqdn_hostname[0] == '\0') {
            if (!repeater_decode_fqdn_labels(option_value + 3, option_len - 3,
                                             fqdn_hostname, sizeof(fqdn_hostname))) {
                repeater_sanitize_hostname_text(option_value + 3, option_len - 3,
                                                fqdn_hostname, sizeof(fqdn_hostname));
            }
        }

        cursor += option_len;
    }

    if (fqdn_hostname[0] != '\0') {
        snprintf(out_hostname, out_hostname_size, "%s", fqdn_hostname);
        return true;
    }

    return false;
}

static int repeater_find_pending_hostname_index_locked(const char *mac)
{
    for (size_t i = 0; i < REPEATER_CLIENT_HISTORY_MAX_ENTRIES; ++i) {
        if (s_pending_client_hostnames[i].occupied &&
            strcmp(s_pending_client_hostnames[i].mac, mac) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int repeater_find_free_pending_hostname_index_locked(void)
{
    for (size_t i = 0; i < REPEATER_CLIENT_HISTORY_MAX_ENTRIES; ++i) {
        if (!s_pending_client_hostnames[i].occupied) {
            return (int)i;
        }
    }

    return -1;
}

static void repeater_cache_pending_hostname(const char *mac, const char *hostname)
{
    int index;

    if (mac == NULL || hostname == NULL || hostname[0] == '\0') {
        return;
    }

    taskENTER_CRITICAL(&s_pending_client_hostnames_lock);
    index = repeater_find_pending_hostname_index_locked(mac);
    if (index < 0) {
        index = repeater_find_free_pending_hostname_index_locked();
    }
    if (index >= 0) {
        s_pending_client_hostnames[(size_t)index].occupied = true;
        snprintf(s_pending_client_hostnames[(size_t)index].mac,
                 sizeof(s_pending_client_hostnames[(size_t)index].mac), "%s", mac);
        snprintf(s_pending_client_hostnames[(size_t)index].hostname,
                 sizeof(s_pending_client_hostnames[(size_t)index].hostname), "%s", hostname);
    }
    taskEXIT_CRITICAL(&s_pending_client_hostnames_lock);
}

s16_t repeater_lwip_dhcps_post_state(struct dhcps_msg *msg, u16_t len, s16_t state)
{
    char mac_text[REPEATER_MAC_STRING_LEN];
    char hostname[REPEATER_CLIENT_HOSTNAME_MAX_LEN + 1];

    if (!repeater_parse_dhcp_hostname(msg, len, hostname, sizeof(hostname))) {
        return state;
    }

    repeater_format_client_mac(mac_text, sizeof(mac_text), msg->chaddr);
    repeater_cache_pending_hostname(mac_text, hostname);
    return state;
}

bool repeater_dhcp_hostnames_get(const char *mac, char *out_hostname, size_t out_hostname_size)
{
    int index;
    bool found = false;

    if (out_hostname == NULL || out_hostname_size == 0) {
        return false;
    }

    out_hostname[0] = '\0';
    if (mac == NULL || mac[0] == '\0') {
        return false;
    }

    taskENTER_CRITICAL(&s_pending_client_hostnames_lock);
    index = repeater_find_pending_hostname_index_locked(mac);
    if (index >= 0) {
        snprintf(out_hostname, out_hostname_size, "%s",
                 s_pending_client_hostnames[(size_t)index].hostname);
        found = out_hostname[0] != '\0';
    }
    taskEXIT_CRITICAL(&s_pending_client_hostnames_lock);

    return found;
}
