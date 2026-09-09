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
static bool namespace_exists, fail_commit;
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
    return 0;
}
