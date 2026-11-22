#include "settings.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NS = "seplos";

static app_settings_t s_settings;
static SemaphoreHandle_t s_lock;
static char s_controller_id[20];

typedef struct {
    const char *key;
    size_t offset;
    size_t size;
} str_field_t;

#define STR_FIELD(k, f) { k, offsetof(app_settings_t, f), sizeof(((app_settings_t *)0)->f) }
static const str_field_t STR_FIELDS[] = {
    STR_FIELD("name", bat_name),
    STR_FIELD("host", hostname),
    STR_FIELD("ssid", wifi_ssid),
    STR_FIELD("wpass", wifi_pass),
    STR_FIELD("mqtt_uri", mqtt_uri),
    STR_FIELD("mqtt_user", mqtt_user),
    STR_FIELD("mqtt_pass", mqtt_pass),
    STR_FIELD("mqtt_base", mqtt_base),
    STR_FIELD("admin_pass", admin_pass),
    STR_FIELD("tz", tz),
};

const char *settings_controller_id(void)
{
    return s_controller_id;
}

static void set_defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->bat_name, "SEPLOS Controller", sizeof(s->bat_name));
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s->hostname, sizeof(s->hostname), "seplos-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s->mqtt_base, sizeof(s->mqtt_base), "Seplos/%s", s_controller_id);
    strlcpy(s->tz, "UTC0", sizeof(s->tz));
    s->mqtt_ha_discovery = true;
    s->allow_remote_shutdown = true;
    s->first_addr = CONFIG_SEPLOS_DEFAULT_FIRST_ADDRESS;
    s->pack_count = CONFIG_SEPLOS_DEFAULT_PACK_COUNT;
    s->poll_ms = CONFIG_SEPLOS_DEFAULT_POLL_MS;
}

esp_err_t settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* ESP8266 ESP.getChipId() = last three MAC bytes, printed as unpadded hex. */
    snprintf(s_controller_id, sizeof(s_controller_id), "SEPLOS_%lx",
             (unsigned long)((mac[3] << 16) | (mac[4] << 8) | mac[5]));

    set_defaults(&s_settings);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < sizeof(STR_FIELDS) / sizeof(STR_FIELDS[0]); i++) {
        size_t len = STR_FIELDS[i].size;
        char *dst = (char *)&s_settings + STR_FIELDS[i].offset;
        char tmp[129];
        size_t tlen = sizeof(tmp);
        if (nvs_get_str(h, STR_FIELDS[i].key, tmp, &tlen) == ESP_OK) {
            strlcpy(dst, tmp, len);
        }
    }
    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "ha", &u8) == ESP_OK) s_settings.mqtt_ha_discovery = u8;
    if (nvs_get_u8(h, "rshut", &u8) == ESP_OK) s_settings.allow_remote_shutdown = u8;
    if (nvs_get_u8(h, "addr", &u8) == ESP_OK && u8 <= 15) s_settings.first_addr = u8;
    if (nvs_get_u8(h, "packs", &u8) == ESP_OK && u8 >= 1 && u8 <= 16) s_settings.pack_count = u8;
    if (nvs_get_u16(h, "poll", &u16) == ESP_OK && u16 >= 500) s_settings.poll_ms = u16;
    nvs_close(h);
    if (s_settings.first_addr + s_settings.pack_count > 16) {
        s_settings.pack_count = (uint8_t)(16 - s_settings.first_addr);
    }
    ESP_LOGI(TAG, "loaded: name='%s' ssid='%s' mqtt='%s' packs=%u@%u", s_settings.bat_name, s_settings.wifi_ssid,
             s_settings.mqtt_uri, s_settings.pack_count, s_settings.first_addr);
    return ESP_OK;
}

void settings_get(app_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

static bool valid_hostname(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 32 || h[0] == '-' || h[n - 1] == '-') {
        return false;
    }
    for (const char *p = h; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t settings_save(const app_settings_t *s, const char **msg)
{
    const char *dummy;
    if (!msg) {
        msg = &dummy;
    }
    *msg = NULL;
    if (s->bat_name[0] == '\0') { *msg = "Battery name must not be empty"; return ESP_ERR_INVALID_ARG; }
    if (!valid_hostname(s->hostname)) { *msg = "Hostname may only contain letters, digits and '-'"; return ESP_ERR_INVALID_ARG; }
    size_t pl = strlen(s->wifi_pass);
    if (pl != 0 && (pl < 8 || pl > 64)) { *msg = "Wi-Fi password must be 8-64 characters (or empty for open networks)"; return ESP_ERR_INVALID_ARG; }
    if (s->mqtt_uri[0] && strncmp(s->mqtt_uri, "mqtt://", 7) != 0 && strncmp(s->mqtt_uri, "mqtts://", 8) != 0 &&
        strncmp(s->mqtt_uri, "ws://", 5) != 0 && strncmp(s->mqtt_uri, "wss://", 6) != 0) {
        *msg = "MQTT broker must start with mqtt://, mqtts://, ws:// or wss://";
        return ESP_ERR_INVALID_ARG;
    }
    if (s->mqtt_base[0] == '\0' || strpbrk(s->mqtt_base, "#+") || s->mqtt_base[strlen(s->mqtt_base) - 1] == '/') {
        *msg = "MQTT base topic must not be empty, contain # or +, or end with /";
        return ESP_ERR_INVALID_ARG;
    }
    if (s->first_addr > 15 || s->pack_count < 1 || s->first_addr + s->pack_count > 16) {
        *msg = "Pack addresses must be within 0..15";
        return ESP_ERR_INVALID_ARG;
    }
    if (s->poll_ms < 500 || s->poll_ms > 60000) { *msg = "Poll interval must be 500-60000 ms"; return ESP_ERR_INVALID_ARG; }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        *msg = "Cannot open settings storage";
        return err;
    }
    for (size_t i = 0; i < sizeof(STR_FIELDS) / sizeof(STR_FIELDS[0]) && err == ESP_OK; i++) {
        err = nvs_set_str(h, STR_FIELDS[i].key, (const char *)s + STR_FIELDS[i].offset);
    }
    if (err == ESP_OK) err = nvs_set_u8(h, "ha", s->mqtt_ha_discovery);
    if (err == ESP_OK) err = nvs_set_u8(h, "rshut", s->allow_remote_shutdown);
    if (err == ESP_OK) err = nvs_set_u8(h, "addr", s->first_addr);
    if (err == ESP_OK) err = nvs_set_u8(h, "packs", s->pack_count);
    if (err == ESP_OK) err = nvs_set_u16(h, "poll", s->poll_ms);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        *msg = "Writing settings failed";
        return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings = *s;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "settings saved");
    return ESP_OK;
}

esp_err_t settings_set_name(const char *name)
{
    app_settings_t s;
    settings_get(&s);
    strlcpy(s.bat_name, name, sizeof(s.bat_name));
    return settings_save(&s, NULL);
}

void settings_get_name(char *out, unsigned size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_settings.bat_name, size);
    xSemaphoreGive(s_lock);
}

static uint8_t flag_get_set(const char *key, int new_value)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return 0;
    }
    nvs_get_u8(h, key, &v);
    if (new_value >= 0 && v != (uint8_t)new_value) {
        nvs_set_u8(h, key, (uint8_t)new_value);
        nvs_commit(h);
    }
    nvs_close(h);
    return v;
}

bool settings_take_portal_request(void)
{
    return flag_get_set("portal", 0) != 0;
}

void settings_request_portal(void)
{
    flag_get_set("portal", 1);
}

static void drd_clear(void *arg)
{
    (void)arg;
    flag_get_set("drd", 0);
}

bool settings_double_reset_detected(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    /* EN/RST button presses show up as power-on or external resets; software restarts,
     * watchdogs and panics never count. */
    bool hw_reset = reason == ESP_RST_POWERON || reason == ESP_RST_EXT || reason == ESP_RST_UNKNOWN;
    uint8_t armed = flag_get_set("drd", hw_reset ? 1 : 0);
    if (hw_reset && armed) {
        flag_get_set("drd", 0);
        ESP_LOGW(TAG, "double reset detected -> setup portal");
        return true;
    }
    if (hw_reset) {
        static esp_timer_handle_t t;
        const esp_timer_create_args_t args = { .callback = drd_clear, .name = "drd" };
        if (esp_timer_create(&args, &t) == ESP_OK) {
            esp_timer_start_once(t, 10 * 1000 * 1000);
        }
    }
    return false;
}
