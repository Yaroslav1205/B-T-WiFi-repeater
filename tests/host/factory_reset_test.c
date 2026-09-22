#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "test_platform.h"
#include "project_wifi_config.h"
#include "repeater_settings.h"
#include "startup_factory_reset.h"

/* In-memory NVS and virtual time: execute the production C modules unchanged. */
typedef struct {
    char key[16];
    unsigned char data[20000];
    size_t size;
} entry_t;
static entry_t saved[32], pending[32];
static bool namespace_exists, fail_commit, fail_history_read;
static unsigned int now_ms, press_ms, release_ms, commits, confirm_flashes;
static bool led_on;

static entry_t *find_entry(entry_t *entries, const char *key)
{
    for (int i = 0; i < 32; ++i)
        if (strcmp(entries[i].key, key) == 0) return &entries[i];
    return NULL;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "repeater_cfg") == 0);
    if (!namespace_exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    namespace_exists = true;
    memcpy(pending, saved, sizeof(saved));
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (fail_commit) return ESP_FAIL;
    memcpy(saved, pending, sizeof(saved));
    ++commits;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    entry_t *entry = find_entry(pending, key);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    memset(entry, 0, sizeof(*entry));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    (void)handle;
    entry_t *entry = find_entry(pending, key);
    if (!entry) entry = find_entry(pending, "");
    assert(entry && strlen(key) < sizeof(entry->key) && size <= sizeof(entry->data));
    strcpy(entry->key, key);
    memcpy(entry->data, data, size);
    entry->size = size;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size)
{
    (void)handle;
    if (fail_history_read && strcmp(key, "client_hist") == 0) return ESP_FAIL;
    entry_t *entry = find_entry(pending, key);
    if (!entry) return ESP_ERR_NVS_NOT_FOUND;
    if (data && *size < entry->size) return ESP_ERR_NVS_INVALID_LENGTH;
    if (data) memcpy(data, entry->data, entry->size);
    *size = entry->size;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *s) { return nvs_get_blob(h, k, v, s); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return nvs_set_blob(h, k, v, strlen(v) + 1); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v) { size_t s = 1; return nvs_get_blob(h, k, v, &s); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *k, uint8_t v) { return nvs_set_blob(h, k, &v, 1); }
esp_err_t nvs_get_i8(nvs_handle_t h, const char *k, int8_t *v) { size_t s = 1; return nvs_get_blob(h, k, v, &s); }
esp_err_t nvs_set_i8(nvs_handle_t h, const char *k, int8_t v) { return nvs_set_blob(h, k, &v, 1); }
esp_err_t esp_wifi_set_max_tx_power(int8_t power) { (void)power; return ESP_OK; }

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    now_ms += ticks * 10;
    assert(now_ms < 20000); /* Catch a startup hang, including waiting for release. */
}
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
int gpio_get_level(int pin)
{
    assert(pin == PROJECT_BOOT_BUTTON_GPIO);
    return now_ms >= press_ms && now_ms < release_ms
        ? PROJECT_BOOT_BUTTON_ACTIVE_LEVEL : !PROJECT_BOOT_BUTTON_ACTIVE_LEVEL;
}
esp_err_t gpio_set_level(int pin, int level)
{
    assert(pin == PROJECT_STATUS_LED_GPIO);
    bool on = level == PROJECT_STATUS_LED_ACTIVE_LEVEL;
    if (on && !led_on && commits > 0) ++confirm_flashes;
    led_on = on;
    return ESP_OK;
}

static void prepare_button(unsigned int press, unsigned int release)
{
    now_ms = commits = confirm_flashes = 0;
    press_ms = press;
    release_ms = release;
    led_on = false;
}
static void seed_custom_settings(void)
{
    assert(repeater_settings_set_wifi_networks("Home", "password1", "", "",
                                               "Hidden", "password2", "wpa2", true) == ESP_OK);
    assert(repeater_settings_set_status_led_enabled(false) == ESP_OK);
    assert(repeater_settings_set_device_description("aa:bb:cc:dd:ee:ff", "Keep me") == ESP_OK);
}
static void assert_custom_settings(void)
{
    assert(repeater_settings_get_softap_config()->ssid_hidden);
    assert(!repeater_settings_is_status_led_enabled());
}
static void assert_defaults(void)
{
    assert(!repeater_settings_get_softap_config()->ssid_hidden);
    assert(repeater_settings_is_status_led_enabled());
    assert(strcmp(repeater_settings_get_softap_config()->ssid, PROJECT_WIFI_AP_SSID) == 0);
    assert(strcmp(repeater_settings_get_station_config()->ssid, PROJECT_WIFI_STA_SSID) == 0);
    assert(strcmp(repeater_settings_get_device_description("aa:bb:cc:dd:ee:ff"), "Keep me") == 0);
}

/* Byte fixtures reproduce the published ESP32 layouts, independently of the
 * production disk structs. Old firmware always stored 64 slots of 88 bytes;
 * 1.0.7 stored count slots of 168 bytes, also labelled version 1. */
static unsigned char history_fixture[8 + 64 * 168 + 1];

static size_t make_history_fixture(bool old_layout, uint8_t version, uint8_t count)
{
    memset(history_fixture, 0, sizeof(history_fixture));
    history_fixture[0] = version;
    history_fixture[1] = count;
    for (size_t i = 0; i < count; ++i) {
        unsigned char *entry = history_fixture + 8 + i * (old_layout ? 88 : 168);
        snprintf((char *)entry, 18, "02:00:00:00:00:%02X", (unsigned int)i);
        strcpy((char *)entry + 18, "Saved client");
        if (!old_layout) {
            strcpy((char *)entry + 67, "saved-host");
            strcpy((char *)entry + 131, "192.168.4.10");
        }
        int64_t first = 1705000000 + (int64_t)i;
        int64_t last = first + 100;
        memcpy(entry + (old_layout ? 72 : 152), &first, 8);
        memcpy(entry + (old_layout ? 80 : 160), &last, 8);
    }
    return old_layout ? 8 + 64 * 88 : 8 + (size_t)count * 168;
}

static void seed_history_fixture(size_t size)
{
    nvs_handle_t handle;
    memset(saved, 0, sizeof(saved));
    memset(pending, 0, sizeof(pending));
    namespace_exists = false;
    fail_commit = fail_history_read = false;
    assert(nvs_open("repeater_cfg", NVS_READWRITE, &handle) == ESP_OK);
    assert(nvs_set_blob(handle, "client_hist", history_fixture, size) == ESP_OK);
    assert(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
    commits = 0;
}

static void assert_history_loaded(bool old_layout, size_t count)
{
    repeater_client_history_entry_t entries[64];
    assert(repeater_settings_get_client_history(entries, 64) == count);
    for (size_t i = 0; i < count; ++i) {
        char mac[18];
        snprintf(mac, sizeof(mac), "02:00:00:00:00:%02X", (unsigned int)i);
        assert(strcmp(entries[i].mac, mac) == 0);
        assert(strcmp(entries[i].description, "Saved client") == 0);
        assert(strcmp(entries[i].hostname, old_layout ? "" : "saved-host") == 0);
        assert(strcmp(entries[i].last_local_ip, old_layout ? "" : "192.168.4.10") == 0);
        assert(entries[i].first_seen_epoch == 1705000000 + (int64_t)i);
        assert(entries[i].last_seen_epoch == 1705000100 + (int64_t)i);
    }
}

static void test_history_upgrade(bool old_layout, uint8_t version, uint8_t count)
{
    size_t size = make_history_fixture(old_layout, version, count);
    seed_history_fixture(size);
    assert(repeater_settings_init() == ESP_OK); /* First boot after OTA. */
    assert_history_loaded(old_layout, count);
    assert(commits == 0); /* Reading/migration does not destroy the old blob. */
    assert(find_entry(saved, "client_hist")->size == size);
    assert(memcmp(find_entry(saved, "client_hist")->data, history_fixture, size) == 0);

    /* First write after OTA must keep every disconnected client. */
    if (count < 64) {
        assert(repeater_settings_record_client_connection("AA:BB:CC:DD:EE:FF") == ESP_OK);
    } else {
        assert(repeater_settings_record_client_connection("AA:BB:CC:DD:EE:FF") == ESP_ERR_NO_MEM);
        assert(repeater_settings_set_device_description("02:00:00:00:00:00", "Saved client") == ESP_OK);
    }
    assert(find_entry(saved, "client_hist")->data[0] == 2);
    assert(repeater_settings_init() == ESP_OK);
    repeater_client_history_entry_t entries[64];
    size_t expected = count < 64 ? count + 1 : count;
    assert(repeater_settings_get_client_history(entries, 64) == expected);
    for (size_t i = 0; i < count; ++i) {
        assert(strcmp(entries[i].description, "Saved client") == 0);
        assert(entries[i].first_seen_epoch == 1705000000 + (int64_t)i);
        assert(entries[i].last_seen_epoch == 1705000100 + (int64_t)i);
        assert(strcmp(entries[i].hostname, old_layout ? "" : "saved-host") == 0);
        assert(strcmp(entries[i].last_local_ip, old_layout ? "" : "192.168.4.10") == 0);
    }
    assert(repeater_settings_factory_reset() == ESP_OK);
    assert(repeater_settings_init() == ESP_OK);
    assert(repeater_settings_get_client_history(entries, 64) == expected);
}

static void assert_unreadable_history_preserved(size_t size)
{
    seed_history_fixture(size);
    assert(repeater_settings_init() == ESP_OK);
    repeater_client_history_entry_t entries[64];
    assert(repeater_settings_get_client_history(entries, 64) == 0);
    for (int reset = 0; reset < 2; ++reset) {
        assert(repeater_settings_record_client_connection("AA:BB:CC:DD:EE:FF") == ESP_ERR_INVALID_STATE);
        assert(repeater_settings_set_client_hostname("AA:BB:CC:DD:EE:FF", "host") == ESP_ERR_INVALID_STATE);
        assert(repeater_settings_set_client_last_local_ip("AA:BB:CC:DD:EE:FF", "192.168.4.2") == ESP_ERR_INVALID_STATE);
        assert(repeater_settings_set_device_description("AA:BB:CC:DD:EE:FF", "label") == ESP_ERR_INVALID_STATE);
        assert(find_entry(saved, "client_hist")->size == size);
        assert(memcmp(find_entry(saved, "client_hist")->data, history_fixture, size) == 0);
        assert(repeater_settings_factory_reset() == ESP_OK);
        assert(repeater_settings_init() == ESP_OK);
    }
}

static void test_history_failures(void)
{
    size_t size = make_history_fixture(false, 99, 2);
    assert_unreadable_history_preserved(size); /* Future schema. */
    size = make_history_fixture(false, 2, 2);
    assert_unreadable_history_preserved(size - 1); /* Truncated record. */
    assert_unreadable_history_preserved(1); /* Truncated header. */
    assert_unreadable_history_preserved(0);
    size = make_history_fixture(false, 2, 2);
    history_fixture[1] = 65;
    assert_unreadable_history_preserved(size);
    size = make_history_fixture(false, 2, 2);
    memset(history_fixture + 8 + 168 + 18, 'x', 49); /* Unterminated second description. */
    assert_unreadable_history_preserved(size);
    size = make_history_fixture(true, 1, 2);
    memset(history_fixture + 8 + 88, 'x', 18); /* Invalid second legacy MAC. */
    assert_unreadable_history_preserved(size);
    size = make_history_fixture(false, 2, 2);
    memcpy(history_fixture + 8 + 168, history_fixture + 8, 18); /* Duplicate MAC. */
    assert_unreadable_history_preserved(size);
    size = make_history_fixture(false, 2, 64);
    assert_unreadable_history_preserved(size + 1); /* Oversized blob. */

    size = make_history_fixture(true, 1, 2);
    seed_history_fixture(size);
    assert(repeater_settings_init() == ESP_OK);
    fail_commit = true;
    assert(repeater_settings_record_client_connection("AA:BB:CC:DD:EE:FF") == ESP_FAIL);
    assert(memcmp(find_entry(saved, "client_hist")->data, history_fixture, size) == 0);
    fail_commit = false;
    assert(repeater_settings_init() == ESP_OK);
    assert_history_loaded(true, 2);

    fail_history_read = true;
    assert(repeater_settings_init() == ESP_FAIL);
    assert(repeater_settings_record_client_connection("AA:BB:CC:DD:EE:FF") == ESP_ERR_INVALID_STATE);
    assert(memcmp(find_entry(saved, "client_hist")->data, history_fixture, size) == 0);
    fail_history_read = false;
    assert(repeater_settings_init() == ESP_OK);
    assert_history_loaded(true, 2);
}

static void test_description_only_upgrade(void)
{
    unsigned char legacy[4 + 24 * 67] = {1, 1, 0, 0};
    nvs_handle_t handle;
    memcpy(legacy + 4, "AA:BB:CC:DD:EE:FF", 18);
    memcpy(legacy + 4 + 18, "Old label", 10);
    memset(saved, 0, sizeof(saved));
    assert(nvs_open("repeater_cfg", NVS_READWRITE, &handle) == ESP_OK);
    assert(nvs_set_blob(handle, "dev_descs", legacy, sizeof(legacy)) == ESP_OK);
    assert(nvs_commit(handle) == ESP_OK);
    nvs_close(handle);
    assert(repeater_settings_init() == ESP_OK);
    assert(strcmp(repeater_settings_get_device_description("AA:BB:CC:DD:EE:FF"), "Old label") == 0);
    assert(repeater_settings_record_client_connection("00:11:22:33:44:55") == ESP_OK);
    assert(repeater_settings_init() == ESP_OK);
    assert(strcmp(repeater_settings_get_device_description("AA:BB:CC:DD:EE:FF"), "Old label") == 0);
}

int main(void)
{
    assert(repeater_settings_init() == ESP_OK);
    assert(!repeater_settings_get_softap_config()->ssid_hidden);
    assert(repeater_settings_is_status_led_enabled());
    seed_custom_settings();
    assert(repeater_settings_init() == ESP_OK);
    assert_custom_settings();

    prepare_button(UINT_MAX, UINT_MAX); /* No press: normal startup, no light. */
    assert(startup_factory_reset_check() == ESP_OK);
    assert(now_ms == PROJECT_FACTORY_RESET_STARTUP_WINDOW_MS && commits == 0 && !led_on);

    prepare_button(1000, 3000); /* Early release cancels and preserves settings. */
    assert(startup_factory_reset_check() == ESP_OK);
    assert(now_ms == 3000 && commits == 0 && confirm_flashes == 0 && !led_on);
    assert_custom_settings();

    prepare_button(1000, 1000 + PROJECT_FACTORY_RESET_HOLD_MS);
    assert(startup_factory_reset_check() == ESP_OK); /* Release exactly at threshold. */
    assert(commits == 0 && confirm_flashes == 0);
    assert_custom_settings();

    prepare_button(PROJECT_FACTORY_RESET_STARTUP_WINDOW_MS - 50, UINT_MAX);
    assert(startup_factory_reset_check() == ESP_OK); /* Late press; full hold; still held. */
    assert(commits == 1 && confirm_flashes == 3 && !led_on);
    assert(now_ms < 16000);
    assert(find_entry(saved, "ap_hidden") == NULL && find_entry(saved, "led_en") == NULL);
    assert_defaults();
    assert(repeater_settings_init() == ESP_OK); /* Reload from NVS after reset. */
    assert_defaults();

    prepare_button(0, UINT_MAX); /* Repeated reset with absent keys is safe. */
    assert(startup_factory_reset_check() == ESP_OK);
    assert(commits == 1 && confirm_flashes == 3);
    assert_defaults();

    seed_custom_settings();
    prepare_button(1000, UINT_MAX);
    fail_commit = true;
    assert(startup_factory_reset_check() == ESP_FAIL);
    assert(commits == 0 && confirm_flashes == 0 && !led_on);
    assert_custom_settings(); /* Failed reset must not change runtime settings. */
    fail_commit = false;
    assert(repeater_settings_init() == ESP_OK);
    assert_custom_settings();

    puts("PASS: startup window, cancellation, reset indication, NVS defaults/reload, history, repeat reset, write failure");
    for (int count = 0; count <= 64; count += (count == 0 ? 2 : 62)) {
        test_history_upgrade(true, 1, (uint8_t)count);
        test_history_upgrade(false, 1, (uint8_t)count);
        test_history_upgrade(false, 2, (uint8_t)count);
    }
    test_history_failures();
    test_description_only_upgrade();
    puts("PASS: history migration, first post-OTA write, reboot/reset, malformed/future blobs, read/write failures");
    return 0;
}
