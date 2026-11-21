#include "mqtt.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bms.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "net.h"
#include "seplos_json.h"
#include "settings.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static app_settings_t s_cfg; /* copy taken at start; MQTT settings change -> reboot */
static bool s_discovery_sent[BMS_MAX_PACKS];

bool mqtt_is_enabled(void)
{
    return s_client != NULL;
}

bool mqtt_is_connected(void)
{
    return s_connected;
}

void mqtt_topic(char *out, size_t size, const char *suffix)
{
    app_settings_t s;
    settings_get(&s);
    snprintf(out, size, "%s/%s", s.mqtt_base, suffix);
}

static void publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!s_client || !s_connected) {
        return;
    }
    int id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
    if (id < 0) {
        ESP_LOGD(TAG, "publish to %s failed", topic);
    }
}

void mqtt_info(const char *msg)
{
    char topic[96];
    snprintf(topic, sizeof topic, "%s/Info", s_cfg.mqtt_base);
    publish(topic, msg, 0, false);
}

static bool payload_is_true(const char *data, int len)
{
    char buf[8];
    if (len <= 0 || len >= (int)sizeof buf) {
        return false;
    }
    memcpy(buf, data, (size_t)len);
    buf[len] = '\0';
    return strcasecmp(buf, "true") == 0 || strcmp(buf, "1") == 0 || strcasecmp(buf, "on") == 0;
}

static void handle_shutdown(uint8_t addr)
{
    char msg[96];
    app_settings_t live; /* this switch can change at runtime */
    settings_get(&live);
    if (!live.allow_remote_shutdown) {
        snprintf(msg, sizeof msg, "Shutdown of pack %u refused: remote shutdown is disabled in settings", addr);
    } else if (bms_request_shutdown(addr) == ESP_OK) {
        snprintf(msg, sizeof msg, "Shutdown of pack %u requested via MQTT", addr);
    } else {
        snprintf(msg, sizeof msg, "Shutdown refused: pack %u is not configured", addr);
    }
    ESP_LOGW(TAG, "%s", msg);
    mqtt_info(msg);
}

static void handle_data(const esp_mqtt_event_t *e)
{
    if (e->total_data_len != e->data_len || e->current_data_offset != 0) {
        return; /* commands are tiny; ignore fragmented payloads */
    }
    char topic[128];
    int tl = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
    memcpy(topic, e->topic, (size_t)tl);
    topic[tl] = '\0';
    if (!payload_is_true(e->data, e->data_len)) {
        return;
    }
    size_t bl = strlen(s_cfg.mqtt_base);
    if (strncmp(topic, s_cfg.mqtt_base, bl) != 0 || topic[bl] != '/') {
        return;
    }
    const char *rest = topic + bl + 1;
    unsigned addr;
    char tail[16];
    if (strcmp(rest, "ShutDown") == 0) {
        /* legacy topic: first configured pack */
        handle_shutdown(s_cfg.first_addr);
    } else if (sscanf(rest, "pack/%u/%15s", &addr, tail) == 2 && strcmp(tail, "shutdown") == 0 && addr < 16) {
        handle_shutdown((uint8_t)addr);
    }
}

static void subscribe_commands(void)
{
    char t[96];
    snprintf(t, sizeof t, "%s/ShutDown", s_cfg.mqtt_base);
    esp_mqtt_client_subscribe_single(s_client, t, 1);
    snprintf(t, sizeof t, "%s/pack/+/shutdown", s_cfg.mqtt_base);
    esp_mqtt_client_subscribe_single(s_client, t, 1);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t e = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        memset(s_discovery_sent, 0, sizeof s_discovery_sent);
        ESP_LOGI(TAG, "connected to %s", s_cfg.mqtt_uri);
        char t[96];
        snprintf(t, sizeof t, "%s/status", s_cfg.mqtt_base);
        publish(t, "online", 1, true);
        subscribe_commands();
        net_info_t ni;
        net_get_info(&ni);
        char msg[128];
        snprintf(msg, sizeof msg, "Seplos Controller is Ready: %s IP: %s", settings_controller_id(), ni.ip);
        mqtt_info(msg);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "disconnected");
        }
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_data(e);
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "broker refused connection (code %d) - check user/password",
                     e->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

/* ---- Home Assistant discovery --------------------------------------------------------- */

typedef struct {
    const char *key;
    const char *name;
    const char *tmpl;
    const char *unit;
    const char *dev_class;
    int precision;
} ha_sensor_t;

static const ha_sensor_t HA_SENSORS[] = {
    { "soc", "State of charge", "{{ value_json.soc }}", "%", "battery", 1 },
    { "soh", "State of health", "{{ value_json.soh }}", "%", NULL, 1 },
    { "voltage", "Voltage", "{{ value_json.voltage }}", "V", "voltage", 2 },
    { "current", "Current", "{{ value_json.current }}", "A", "current", 2 },
    { "power", "Power", "{{ value_json.power }}", "W", "power", 0 },
    { "remaining", "Remaining capacity", "{{ value_json.remaining_ah }}", "Ah", NULL, 2 },
    { "capacity", "Full capacity", "{{ value_json.capacity_ah }}", "Ah", NULL, 2 },
    { "cycles", "Cycles", "{{ value_json.cycles }}", NULL, NULL, 0 },
    { "port_voltage", "Port voltage", "{{ value_json.port_voltage }}", "V", "voltage", 2 },
    { "cell_min", "Cell min", "{{ value_json.cell_min_v }}", "V", "voltage", 3 },
    { "cell_max", "Cell max", "{{ value_json.cell_max_v }}", "V", "voltage", 3 },
    { "cell_diff", "Cell difference", "{{ value_json.cell_diff_mv }}", "mV", "voltage", 0 },
    { "temp_ambient", "Ambient temperature", "{{ value_json.temps.ambient }}", "\xC2\xB0" "C", "temperature", 1 },
    { "temp_power", "MOS temperature", "{{ value_json.temps.power }}", "\xC2\xB0" "C", "temperature", 1 },
    { "alarms", "Active alarms", "{{ value_json.alarm_count | default(0) }}", NULL, NULL, 0 },
};

static void ha_publish_sensor(sbuf_t *sb, char *buf, size_t cap, const seplos_pack_state_t *p, const char *key,
                              const char *name, const char *tmpl, const char *unit, const char *dev_class,
                              int precision, size_t pack_count)
{
    char topic[128];
    const char *id = settings_controller_id();
    char devname[64];
    char batname[SETTINGS_NAME_LEN];
    settings_get_name(batname, sizeof batname);
    if (pack_count > 1) {
        snprintf(devname, sizeof devname, "%s pack %u", batname, p->address);
    } else {
        strlcpy(devname, batname, sizeof devname);
    }
    sb_init(sb, buf, cap);
    sb_puts(sb, "{\"name\":");
    sb_json_str(sb, name);
    sb_printf(sb, ",\"unique_id\":\"%s_p%u_%s\",\"object_id\":\"%s_p%u_%s\"", id, p->address, key, id, p->address, key);
    sb_printf(sb, ",\"state_topic\":\"%s/pack/%u/state\",\"value_template\":", s_cfg.mqtt_base, p->address);
    sb_json_str(sb, tmpl);
    sb_printf(sb, ",\"availability_topic\":\"%s/status\",\"expire_after\":120", s_cfg.mqtt_base);
    if (unit) {
        sb_puts(sb, ",\"unit_of_measurement\":");
        sb_json_str(sb, unit);
    }
    if (dev_class) {
        sb_printf(sb, ",\"device_class\":\"%s\"", dev_class);
    }
    sb_printf(sb, ",\"state_class\":\"measurement\",\"suggested_display_precision\":%d", precision);
    sb_printf(sb, ",\"device\":{\"identifiers\":[\"%s_p%u\"],\"name\":", id, p->address);
    sb_json_str(sb, devname);
    sb_printf(sb, ",\"manufacturer\":\"Seplos\",\"model\":\"BMS (protocol V2.0, address %u)\",\"sw_version\":\"%s\"}}",
              p->address, esp_app_get_description()->version);
    if (sb->overflow) {
        ESP_LOGW(TAG, "discovery payload too large for %s", key);
        return;
    }
    snprintf(topic, sizeof topic, "homeassistant/sensor/%s_p%u_%s/config", id, p->address, key);
    publish(topic, buf, 1, true);
}

static void ha_discovery(const seplos_pack_state_t *p, size_t pack_count)
{
    const size_t cap = 1024;
    char *buf = malloc(cap);
    if (!buf) {
        return;
    }
    sbuf_t sb;
    for (size_t i = 0; i < sizeof(HA_SENSORS) / sizeof(HA_SENSORS[0]); i++) {
        const ha_sensor_t *s = &HA_SENSORS[i];
        ha_publish_sensor(&sb, buf, cap, p, s->key, s->name, s->tmpl, s->unit, s->dev_class, s->precision, pack_count);
    }
    char key[24], name[32], tmpl[64];
    for (unsigned c = 0; c < p->tele.cell_count; c++) {
        snprintf(key, sizeof key, "cell%u", c + 1);
        snprintf(name, sizeof name, "Cell %u", c + 1);
        snprintf(tmpl, sizeof tmpl, "{{ value_json.cells[%u] }}", c);
        ha_publish_sensor(&sb, buf, cap, p, key, name, tmpl, "V", "voltage", 3, pack_count);
    }
    unsigned cell_temps = 0;
    for (uint8_t t = 0; t < p->tele.temp_count; t++) {
        cell_temps += seplos_temp_kind(t, p->tele.temp_count) == SEPLOS_TEMP_CELL;
    }
    for (unsigned t = 0; t < cell_temps; t++) {
        snprintf(key, sizeof key, "cell_temp%u", t + 1);
        snprintf(name, sizeof name, "Cell temperature %u", t + 1);
        snprintf(tmpl, sizeof tmpl, "{{ value_json.temps.cells[%u] }}", t);
        ha_publish_sensor(&sb, buf, cap, p, key, name, tmpl, "\xC2\xB0" "C", "temperature", 1, pack_count);
    }
    free(buf);
    ESP_LOGI(TAG, "Home Assistant discovery published for pack %u", p->address);
}

/* ---- periodic publisher ----------------------------------------------------------------- */

static void publisher_task(void *arg)
{
    (void)arg;
    seplos_pack_state_t *packs = calloc(BMS_MAX_PACKS, sizeof(*packs));
    const size_t cap = 4096;
    char *buf = malloc(cap);
    char topic[128];
    bool was_online[BMS_MAX_PACKS] = { 0 };
    uint32_t seen = 0;
    assert(packs && buf);
    for (;;) {
        seen = bms_wait_cycle(seen, 5000);
        if (!s_connected) {
            continue;
        }
        size_t n = bms_snapshot(packs, BMS_MAX_PACKS);
        char batname[SETTINGS_NAME_LEN];
        settings_get_name(batname, sizeof batname);
        sbuf_t sb;
        for (size_t i = 0; i < n; i++) {
            const seplos_pack_state_t *p = &packs[i];
            if (p->online != was_online[i]) {
                was_online[i] = p->online;
                char msg[96];
                snprintf(msg, sizeof msg, p->online ? "BMS pack %u online" : "BMS pack %u offline: %s", p->address,
                         seplos_err_str(p->last_err));
                mqtt_info(msg);
            }
            if (!p->has_data) {
                continue;
            }
            if (i == 0) {
                /* Original firmware payload and topic: <base>/AllJsonData */
                sb_init(&sb, buf, cap);
                seplos_json_legacy(&sb, p, batname);
                snprintf(topic, sizeof topic, "%s/AllJsonData", s_cfg.mqtt_base);
                if (!sb.overflow) {
                    publish(topic, buf, 0, false);
                }
            }
            sb_init(&sb, buf, cap);
            seplos_json_pack(&sb, p);
            snprintf(topic, sizeof topic, "%s/pack/%u/state", s_cfg.mqtt_base, p->address);
            if (!sb.overflow) {
                publish(topic, buf, 0, false);
            }
            if (s_cfg.mqtt_ha_discovery && !s_discovery_sent[i] && p->online) {
                s_discovery_sent[i] = true;
                ha_discovery(p, n);
            }
        }
    }
}

esp_err_t mqtt_start(void)
{
    settings_get(&s_cfg);
    if (s_cfg.mqtt_uri[0] == '\0') {
        ESP_LOGI(TAG, "no MQTT broker configured");
        return ESP_OK;
    }
    static char lwt_topic[96];
    snprintf(lwt_topic, sizeof lwt_topic, "%s/status", s_cfg.mqtt_base);
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_cfg.mqtt_uri,
        .credentials.client_id = settings_controller_id(),
        .credentials.username = s_cfg.mqtt_user[0] ? s_cfg.mqtt_user : NULL,
        .credentials.authentication.password = s_cfg.mqtt_pass[0] ? s_cfg.mqtt_pass : NULL,
        .session.last_will = { .topic = lwt_topic, .msg = "offline", .qos = 1, .retain = 1 },
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 5000,
        .buffer.size = 2048,
        .buffer.out_size = 4096,
        .task.stack_size = 6144,
        .outbox.limit = 16 * 1024, /* bounded memory if the broker stalls */
    };
    if (strncmp(s_cfg.mqtt_uri, "mqtts://", 8) == 0 || strncmp(s_cfg.mqtt_uri, "wss://", 6) == 0) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        return err;
    }
    xTaskCreate(publisher_task, "mqtt_pub", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "MQTT enabled: %s, base topic %s", s_cfg.mqtt_uri, s_cfg.mqtt_base);
    return ESP_OK;
}
