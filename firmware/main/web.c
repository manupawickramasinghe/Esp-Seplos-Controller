#include "web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bms.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt.h"
#include "net.h"
#include "sdkconfig.h"
#include "seplos_json.h"
#include "settings.h"

static const char *TAG = "web";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

static httpd_handle_t s_server;
static esp_timer_handle_t s_restart_timer;

/* ---- helpers ------------------------------------------------------------------------- */

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void schedule_restart(uint32_t ms)
{
    if (!s_restart_timer) {
        const esp_timer_create_args_t a = { .callback = restart_cb, .name = "restart" };
        esp_timer_create(&a, &s_restart_timer);
    }
    esp_timer_stop(s_restart_timer);
    esp_timer_start_once(s_restart_timer, (uint64_t)ms * 1000);
}

static void no_cache(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    no_cache(req);
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char buf[200];
    sbuf_t sb;
    sb_init(&sb, buf, sizeof buf);
    sb_puts(&sb, "{\"ok\":false,\"error\":");
    sb_json_str(&sb, msg);
    sb_putc(&sb, '}');
    httpd_resp_set_status(req, status);
    return send_json(req, buf);
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64val(*in);
        if (v < 0) {
            return 0;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= cap) {
                return 0;
            }
            out[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[o] = '\0';
    return o;
}

/* HTTP Basic auth (user "admin") for state-changing endpoints, when a password is set. */
static bool authorized(httpd_req_t *req)
{
    app_settings_t s;
    settings_get(&s);
    if (s.admin_pass[0] == '\0') {
        return true;
    }
    char hdr[160];
    char dec[140] = { 0 };
    bool ok = false;
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0 && b64decode(hdr + 6, dec, sizeof dec) > 0) {
        char expect[80];
        snprintf(expect, sizeof expect, "admin:%s", s.admin_pass);
        size_t n = strlen(expect);
        unsigned diff = (unsigned)(strlen(dec) ^ n);
        for (size_t i = 0; i < n; i++) {
            diff |= (unsigned)(dec[i < sizeof(dec) ? i : 0] ^ expect[i]);
        }
        ok = diff == 0;
    }
    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Seplos Controller\"");
        httpd_resp_sendstr(req, "Authentication required");
    }
    return ok;
}

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* Read a small urlencoded body into a heap buffer (caller frees). */
static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len > max) {
        send_error(req, "413 Payload Too Large", "request too large");
        return NULL;
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        send_error(req, "500 Internal Server Error", "out of memory");
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(body);
            return NULL;
        }
        got += (size_t)r;
    }
    return body;
}

/* Get a decoded form field. Returns false if absent. */
static bool form_get(const char *body, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(body, key, out, size) != ESP_OK) {
        return false;
    }
    url_decode(out);
    return true;
}

/* ---- pages --------------------------------------------------------------------------- */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    no_cache(req);
    return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
}

static void device_json(sbuf_t *sb)
{
    net_info_t ni;
    net_get_info(&ni);
    char name[SETTINGS_NAME_LEN];
    settings_get_name(name, sizeof name);
    app_settings_t s;
    settings_get(&s);
    time_t boot = net_boot_time();
    char started[32] = "";
    if (boot) {
        struct tm tm;
        localtime_r(&boot, &tm);
        strftime(started, sizeof started, "%Y-%m-%d %H:%M:%S", &tm);
    }
    char shutdown[96];
    bms_last_shutdown_result(shutdown, sizeof shutdown);

    sb_puts(sb, "{\"name\":");
    sb_json_str(sb, name);
    sb_puts(sb, ",\"id\":");
    sb_json_str(sb, settings_controller_id());
    sb_puts(sb, ",\"version\":");
    sb_json_str(sb, esp_app_get_description()->version);
    sb_puts(sb, ",\"idf\":");
    sb_json_str(sb, esp_app_get_description()->idf_ver);
    sb_printf(sb, ",\"uptime_s\":%lu,\"free_heap\":%lu,\"min_free_heap\":%lu",
              (unsigned long)(esp_timer_get_time() / 1000000), (unsigned long)esp_get_free_heap_size(),
              (unsigned long)esp_get_minimum_free_heap_size());
    sb_puts(sb, ",\"started\":");
    sb_json_str(sb, started);
    sb_printf(sb, ",\"wifi\":{\"connected\":%s,\"portal\":%s,\"ssid\":", ni.connected ? "true" : "false",
              ni.portal ? "true" : "false");
    sb_json_str(sb, ni.ssid);
    sb_printf(sb, ",\"rssi\":%d,\"ip\":", ni.rssi);
    sb_json_str(sb, ni.ip);
    sb_puts(sb, ",\"ap_ssid\":");
    sb_json_str(sb, ni.ap_ssid);
    sb_puts(sb, ",\"hostname\":");
    sb_json_str(sb, ni.hostname);
    sb_printf(sb, "},\"mqtt\":{\"enabled\":%s,\"connected\":%s,\"base\":", mqtt_is_enabled() ? "true" : "false",
              mqtt_is_connected() ? "true" : "false");
    sb_json_str(sb, s.mqtt_base);
    sb_printf(sb, ",\"remote_shutdown\":%s}", s.allow_remote_shutdown ? "true" : "false");
    sb_printf(sb, ",\"auth\":%s,\"poll_ms\":%u,\"last_shutdown\":", s.admin_pass[0] ? "true" : "false", s.poll_ms);
    sb_json_str(sb, shutdown);
    sb_putc(sb, '}');
}

static esp_err_t api_data_get(httpd_req_t *req)
{
    seplos_pack_state_t *packs = calloc(BMS_MAX_PACKS, sizeof(*packs));
    char *buf = malloc(4096);
    if (!packs || !buf) {
        free(packs);
        free(buf);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t n = bms_snapshot(packs, BMS_MAX_PACKS);
    httpd_resp_set_type(req, "application/json");
    no_cache(req);
    sbuf_t sb;
    sb_init(&sb, buf, 4096);
    sb_puts(&sb, "{\"device\":");
    device_json(&sb);
    sb_puts(&sb, ",\"packs\":[");
    esp_err_t err = httpd_resp_send_chunk(req, buf, (ssize_t)sb.len);
    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        sb_init(&sb, buf, 4096);
        if (i) {
            sb_putc(&sb, ',');
        }
        seplos_json_pack(&sb, &packs[i]);
        err = httpd_resp_send_chunk(req, buf, (ssize_t)sb.len);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    free(packs);
    free(buf);
    return err;
}

/* Original firmware endpoint: same JSON as v1.0.1.2 for the first (or ?address=N) pack. */
static esp_err_t legacy_alljson_get(httpd_req_t *req)
{
    seplos_pack_state_t *packs = calloc(BMS_MAX_PACKS, sizeof(*packs));
    char *buf = malloc(2048);
    if (!packs || !buf) {
        free(packs);
        free(buf);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t n = bms_snapshot(packs, BMS_MAX_PACKS);
    size_t idx = 0;
    char q[32], v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "address", v, sizeof v) == ESP_OK) {
        int a = atoi(v);
        for (size_t i = 0; i < n; i++) {
            if (packs[i].address == a) {
                idx = i;
            }
        }
    }
    char name[SETTINGS_NAME_LEN];
    settings_get_name(name, sizeof name);
    sbuf_t sb;
    sb_init(&sb, buf, 2048);
    if (n) {
        seplos_json_legacy(&sb, &packs[idx], name);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t err = send_json(req, buf);
    free(packs);
    free(buf);
    return err;
}

/* Original firmware endpoint "dataonce" (static info shown in the old UI). */
static esp_err_t legacy_dataonce_get(httpd_req_t *req)
{
    net_info_t ni;
    net_get_info(&ni);
    char name[SETTINGS_NAME_LEN];
    settings_get_name(name, sizeof name);
    char t_shut[96], t_info[96], t_all[96], url[96], started[32] = "";
    mqtt_topic(t_shut, sizeof t_shut, "ShutDown");
    mqtt_topic(t_info, sizeof t_info, "Info");
    mqtt_topic(t_all, sizeof t_all, "AllJsonData");
    time_t boot = net_boot_time();
    if (boot) {
        struct tm tm;
        localtime_r(&boot, &tm);
        strftime(started, sizeof started, "%a %b %d %H:%M:%S %Y", &tm);
    }
    char buf[1024];
    sbuf_t sb;
    sb_init(&sb, buf, sizeof buf);
    sb_puts(&sb, "{\"Version\":");
    sb_json_str(&sb, esp_app_get_description()->version);
    sb_puts(&sb, ",\"BatName\":");
    sb_json_str(&sb, name);
    sb_puts(&sb, ",\"CONTROLLERID\":");
    sb_json_str(&sb, settings_controller_id());
    sb_printf(&sb, ",\"WIFI_RSSI\":\"%d\",\"WIFI_SSID\":", ni.rssi);
    sb_json_str(&sb, ni.ssid);
    sb_puts(&sb, ",\"StartTime\":");
    sb_json_str(&sb, started);
    snprintf(url, sizeof url, "http://%s/AllJsonData", ni.ip);
    sb_puts(&sb, ",\"Url_Data\":");
    sb_json_str(&sb, url);
    snprintf(url, sizeof url, "http://%s/api/shutdown (POST)", ni.ip);
    sb_puts(&sb, ",\"Url_ShutDown\":");
    sb_json_str(&sb, url);
    sb_puts(&sb, ",\"Topic_ShutDown\":");
    sb_json_str(&sb, t_shut);
    sb_puts(&sb, ",\"Topic_Info\":");
    sb_json_str(&sb, t_info);
    sb_puts(&sb, ",\"Topic_AllJsonData\":");
    sb_json_str(&sb, t_all);
    sb_putc(&sb, '}');
    return send_json(req, buf);
}

static esp_err_t api_config_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    app_settings_t s;
    settings_get(&s);
    char buf[1024];
    sbuf_t sb;
    sb_init(&sb, buf, sizeof buf);
    sb_puts(&sb, "{\"name\":");
    sb_json_str(&sb, s.bat_name);
    sb_puts(&sb, ",\"hostname\":");
    sb_json_str(&sb, s.hostname);
    sb_puts(&sb, ",\"wifi_ssid\":");
    sb_json_str(&sb, s.wifi_ssid);
    sb_printf(&sb, ",\"wifi_pass_set\":%s,\"mqtt_uri\":", s.wifi_pass[0] ? "true" : "false");
    sb_json_str(&sb, s.mqtt_uri);
    sb_puts(&sb, ",\"mqtt_user\":");
    sb_json_str(&sb, s.mqtt_user);
    sb_printf(&sb, ",\"mqtt_pass_set\":%s,\"mqtt_base\":", s.mqtt_pass[0] ? "true" : "false");
    sb_json_str(&sb, s.mqtt_base);
    sb_printf(&sb, ",\"ha\":%s,\"remote_shutdown\":%s,\"admin_pass_set\":%s,\"tz\":", s.mqtt_ha_discovery ? "true" : "false",
              s.allow_remote_shutdown ? "true" : "false", s.admin_pass[0] ? "true" : "false");
    sb_json_str(&sb, s.tz);
    sb_printf(&sb, ",\"first_addr\":%u,\"pack_count\":%u,\"poll_ms\":%u}", s.first_addr, s.pack_count, s.poll_ms);
    return send_json(req, buf);
}

static esp_err_t api_config_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char *body = read_body(req, 2048);
    if (!body) {
        return ESP_OK;
    }
    app_settings_t old, s;
    settings_get(&old);
    s = old;
    char v[400]; /* URL-encoded values can be 3x their decoded length */
    if (form_get(body, "name", v, sizeof v)) strlcpy(s.bat_name, v, sizeof s.bat_name);
    if (form_get(body, "hostname", v, sizeof v)) strlcpy(s.hostname, v, sizeof s.hostname);
    if (form_get(body, "wifi_ssid", v, sizeof v)) strlcpy(s.wifi_ssid, v, sizeof s.wifi_ssid);
    /* Secrets: empty field = keep; *_clear=1 = remove. */
    if (form_get(body, "wifi_pass", v, sizeof v) && v[0]) strlcpy(s.wifi_pass, v, sizeof s.wifi_pass);
    if (form_get(body, "wifi_pass_clear", v, sizeof v) && v[0] == '1') s.wifi_pass[0] = '\0';
    if (form_get(body, "mqtt_uri", v, sizeof v)) strlcpy(s.mqtt_uri, v, sizeof s.mqtt_uri);
    if (form_get(body, "mqtt_user", v, sizeof v)) strlcpy(s.mqtt_user, v, sizeof s.mqtt_user);
    if (form_get(body, "mqtt_pass", v, sizeof v) && v[0]) strlcpy(s.mqtt_pass, v, sizeof s.mqtt_pass);
    if (form_get(body, "mqtt_pass_clear", v, sizeof v) && v[0] == '1') s.mqtt_pass[0] = '\0';
    if (form_get(body, "mqtt_base", v, sizeof v)) strlcpy(s.mqtt_base, v, sizeof s.mqtt_base);
    if (form_get(body, "ha", v, sizeof v)) s.mqtt_ha_discovery = v[0] == '1';
    if (form_get(body, "remote_shutdown", v, sizeof v)) s.allow_remote_shutdown = v[0] == '1';
    if (form_get(body, "admin_pass", v, sizeof v) && v[0]) strlcpy(s.admin_pass, v, sizeof s.admin_pass);
    if (form_get(body, "admin_pass_clear", v, sizeof v) && v[0] == '1') s.admin_pass[0] = '\0';
    if (form_get(body, "tz", v, sizeof v) && v[0]) strlcpy(s.tz, v, sizeof s.tz);
    if (form_get(body, "first_addr", v, sizeof v)) s.first_addr = (uint8_t)atoi(v);
    if (form_get(body, "pack_count", v, sizeof v)) s.pack_count = (uint8_t)atoi(v);
    if (form_get(body, "poll_ms", v, sizeof v)) s.poll_ms = (uint16_t)atoi(v);
    free(body);

    const char *msg = NULL;
    if (settings_save(&s, &msg) != ESP_OK) {
        return send_error(req, "400 Bad Request", msg ? msg : "invalid settings");
    }
    /* Name, time zone, admin password and the remote-shutdown switch apply immediately;
     * everything else (network, MQTT, BMS addresses) is applied at boot. */
    app_settings_t cmp = old;
    strlcpy(cmp.bat_name, s.bat_name, sizeof cmp.bat_name);
    strlcpy(cmp.tz, s.tz, sizeof cmp.tz);
    strlcpy(cmp.admin_pass, s.admin_pass, sizeof cmp.admin_pass);
    cmp.allow_remote_shutdown = s.allow_remote_shutdown;
    bool restart = memcmp(&cmp, &s, sizeof s) != 0;
    setenv("TZ", s.tz, 1);
    tzset();
    if (restart) {
        schedule_restart(1500);
    }
    return send_json(req, restart ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true,\"restart\":false}");
}

/* Original firmware: GET /System?BatName=... saved the battery name. */
static esp_err_t legacy_system_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char q[128], v[SETTINGS_NAME_LEN * 3];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "BatName", v, sizeof v) == ESP_OK) {
        url_decode(v);
        if (v[0] && settings_set_name(v) == ESP_OK) {
            return send_json(req, "{\"ok\":true}");
        }
    }
    return send_error(req, "400 Bad Request", "BatName missing");
}

static esp_err_t api_scan_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char *buf = malloc(2048);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    sbuf_t sb;
    sb_init(&sb, buf, 2048);
    net_scan_json(&sb);
    esp_err_t err = send_json(req, sb.overflow ? "[]" : buf);
    free(buf);
    return err;
}

static esp_err_t api_restart_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char *body = read_body(req, 256);
    char v[8] = "";
    if (body) {
        if (form_get(body, "portal", v, sizeof v) && v[0] == '1') {
            settings_request_portal();
        }
        free(body);
    }
    schedule_restart(1000);
    return send_json(req, "{\"ok\":true,\"restart\":true}");
}

static esp_err_t do_shutdown(httpd_req_t *req, uint8_t addr)
{
    esp_err_t err = bms_request_shutdown(addr);
    if (err != ESP_OK) {
        return send_error(req, "404 Not Found", "pack address is not configured");
    }
    ESP_LOGW(TAG, "shutdown of pack %u requested via web", addr);
    mqtt_info("Shutdown requested via web interface");
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t api_shutdown_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    char *body = read_body(req, 256);
    if (!body) {
        return ESP_OK;
    }
    char addr[8] = "", confirm[16] = "";
    form_get(body, "address", addr, sizeof addr);
    form_get(body, "confirm", confirm, sizeof confirm);
    free(body);
    if (strcmp(confirm, "SHUTDOWN") != 0) {
        return send_error(req, "400 Bad Request", "confirm=SHUTDOWN is required");
    }
    if (!isdigit((unsigned char)addr[0])) {
        return send_error(req, "400 Bad Request", "address is required");
    }
    return do_shutdown(req, (uint8_t)atoi(addr));
}

#if CONFIG_SEPLOS_LEGACY_GET_SHUTDOWN
static esp_err_t legacy_shutdown_get(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    app_settings_t s;
    settings_get(&s);
    return do_shutdown(req, s.first_addr);
}
#endif

static esp_err_t api_ota_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        return ESP_OK;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_error(req, "500 Internal Server Error", "no OTA partition");
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        return send_error(req, "400 Bad Request", "firmware size invalid");
    }
    char *buf = malloc(4096);
    if (!buf) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    ESP_LOGI(TAG, "OTA: %u bytes -> %s", (unsigned)req->content_len, part->label);
    esp_ota_handle_t ota = 0;
    esp_err_t err = ESP_OK;
    size_t got = 0;
    bool begun = false;
    const char *fail = NULL;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf, 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            fail = "upload interrupted";
            break;
        }
        if (!begun) {
            if ((uint8_t)buf[0] != 0xE9) {
                fail = "not an ESP firmware image (.bin)";
                break;
            }
            err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
            if (err != ESP_OK) {
                fail = "cannot start update";
                break;
            }
            begun = true;
        }
        err = esp_ota_write(ota, buf, (size_t)r);
        if (err != ESP_OK) {
            fail = "flash write failed";
            break;
        }
        got += (size_t)r;
    }
    free(buf);
    if (!fail && begun) {
        err = esp_ota_end(ota);
        begun = false;
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            fail = "image validation failed (wrong chip or corrupted file)";
        } else if (err != ESP_OK) {
            fail = "finishing update failed";
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            fail = "cannot select new firmware";
        }
    }
    if (begun) {
        esp_ota_abort(ota);
    }
    if (fail) {
        ESP_LOGE(TAG, "OTA failed: %s (%s)", fail, esp_err_to_name(err));
        return send_error(req, "400 Bad Request", fail);
    }
    ESP_LOGI(TAG, "OTA done, restarting");
    schedule_restart(1500);
    return send_json(req, "{\"ok\":true,\"restart\":true}");
}

/* Captive portal: unknown URLs redirect to the UI while the setup AP is running. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    (void)code;
    net_info_t ni;
    net_get_info(&ni);
    if (ni.portal) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;
    cfg.recv_wait_timeout = 15;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get },
        { .uri = "/api/data", .method = HTTP_GET, .handler = api_data_get },
        { .uri = "/api/config", .method = HTTP_GET, .handler = api_config_get },
        { .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = api_scan_get },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = api_restart_post },
        { .uri = "/api/shutdown", .method = HTTP_POST, .handler = api_shutdown_post },
        { .uri = "/api/ota", .method = HTTP_POST, .handler = api_ota_post },
        /* original firmware endpoints */
        { .uri = "/AllJsonData", .method = HTTP_GET, .handler = legacy_alljson_get },
        { .uri = "/dataonce", .method = HTTP_GET, .handler = legacy_dataonce_get },
        { .uri = "/System", .method = HTTP_GET, .handler = legacy_system_get },
#if CONFIG_SEPLOS_LEGACY_GET_SHUTDOWN
        { .uri = "/ShutDown", .method = HTTP_GET, .handler = legacy_shutdown_get },
#endif
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}
