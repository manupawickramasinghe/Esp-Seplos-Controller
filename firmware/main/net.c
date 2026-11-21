#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "dns_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "settings.h"

#if CONFIG_SEPLOS_QEMU_OPENETH
#include "esp_eth.h"
#endif

static const char *TAG = "net";

/* Start the fallback setup AP when the station could not connect for this long. */
#define FALLBACK_AP_AFTER_MS (3 * 60 * 1000)

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static SemaphoreHandle_t s_lock;
static net_info_t s_info;
static bool s_portal_forced;
static bool s_sta_enabled;
static int s_retry;
static int64_t s_disconnected_since;
static esp_timer_handle_t s_reconnect_timer;
static net_ip_cb_t s_ip_cb;
static bool s_sntp_started;
static time_t s_boot_time;

void net_on_connected(net_ip_cb_t cb)
{
    s_ip_cb = cb;
}

bool net_is_connected(void)
{
    return s_info.connected;
}

void net_get_info(net_info_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    xSemaphoreGive(s_lock);
    if (out->connected && s_sta) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

time_t net_boot_time(void)
{
    if (s_boot_time == 0) {
        time_t now = time(NULL);
        if (now > 1700000000) { /* clock is set */
            s_boot_time = now - (time_t)(esp_timer_get_time() / 1000000);
        }
    }
    return s_boot_time;
}

static void start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.wait_for_sync = false;
    esp_netif_sntp_init(&cfg);
}

static void start_mdns(void)
{
    static bool started;
    if (started) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    started = true;
    char name[SETTINGS_NAME_LEN];
    settings_get_name(name, sizeof(name));
    mdns_hostname_set(s_info.hostname);
    mdns_instance_name_set(name);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local/", s_info.hostname);
}

static void got_ip(const esp_ip4_addr_t *ip)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.connected = true;
    snprintf(s_info.ip, sizeof(s_info.ip), IPSTR, IP2STR(ip));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "connected, IP %s", s_info.ip);
    start_sntp();
    start_mdns();
    if (s_ip_cb) {
        s_ip_cb();
    }
}

static void start_ap(void)
{
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_info.ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_info.ap_ssid);
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    const char *pw = CONFIG_SEPLOS_AP_PASSWORD;
    if (strlen(pw) >= 8) {
        strlcpy((char *)ap.ap.password, pw, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }
    /* Always AP+STA: Wi-Fi scanning for the settings page needs the station interface. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    esp_netif_ip_info_t ipi;
    esp_netif_get_ip_info(s_ap, &ipi);
    dns_server_start(ipi.ip.addr);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.portal = true;
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "setup portal: join Wi-Fi '%s' (password '%s') and open http://" IPSTR "/", s_info.ap_ssid, pw,
             IP2STR(&ipi.ip));
}

static void stop_fallback_ap(void)
{
    if (!s_info.portal || s_portal_forced) {
        return;
    }
    dns_server_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info.portal = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "station connected, setup access point stopped");
}

/* A forced setup portal (button / double reset) with stored Wi-Fi gives up after a while when
 * nobody uses it, so an unattended controller recovers from e.g. two quick power blips. */
#define PORTAL_IDLE_TIMEOUT_US (5ULL * 60 * 1000 * 1000)
static esp_timer_handle_t s_portal_timer;

__attribute__((unused)) static void portal_timeout_cb(void *arg)
{
    (void)arg;
    wifi_sta_list_t clients = { 0 };
    if (esp_wifi_ap_get_sta_list(&clients) == ESP_OK && clients.num > 0) {
        esp_timer_start_once(s_portal_timer, PORTAL_IDLE_TIMEOUT_US); /* someone is configuring: wait */
        return;
    }
    ESP_LOGW(TAG, "setup portal unused for 5 min, restarting with the saved Wi-Fi settings");
    esp_restart();
}

__attribute__((unused)) static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

__attribute__((unused)) static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_enabled) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        bool was = s_info.connected;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info.connected = false;
        s_info.ip[0] = '\0';
        xSemaphoreGive(s_lock);
        if (was || s_disconnected_since == 0) {
            s_disconnected_since = esp_timer_get_time();
        }
        if (!s_sta_enabled) {
            return;
        }
        int delay_s = 1 << (s_retry < 5 ? s_retry : 5); /* 1,2,4..32 s */
        s_retry++;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retry in %d s", d ? d->reason : -1, delay_s);
        if (!s_info.portal && (esp_timer_get_time() - s_disconnected_since) / 1000 > FALLBACK_AP_AFTER_MS) {
            ESP_LOGW(TAG, "no Wi-Fi for %d min, starting setup access point as fallback", FALLBACK_AP_AFTER_MS / 60000);
            start_ap();
        }
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_s * 1000000ULL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_retry = 0;
        s_disconnected_since = 0;
        got_ip(&e->ip_info.ip);
        stop_fallback_ap();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "client joined the setup access point");
    }
}

#if CONFIG_SEPLOS_QEMU_OPENETH
static void eth_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *e = data;
    got_ip(&e->ip_info.ip);
}

static esp_err_t start_openeth(void)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = -1;
    phy_cfg.autonego_timeout_ms = 100;
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&cfg, &eth));
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&ncfg);
    esp_netif_set_hostname(netif, s_info.hostname);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, eth_got_ip, NULL));
    strlcpy(s_info.ssid, "qemu-openeth", sizeof(s_info.ssid));
    ESP_LOGW(TAG, "QEMU build: using open_eth instead of Wi-Fi");
    return esp_eth_start(eth);
}
#endif

esp_err_t net_start(bool force_portal)
{
    s_lock = xSemaphoreCreateMutex();
    app_settings_t st;
    settings_get(&st);
    strlcpy(s_info.hostname, st.hostname, sizeof(s_info.hostname));
    /* Same SSID scheme as the original firmware: SEPLOS_<chip id>. */
    strlcpy(s_info.ap_ssid, settings_controller_id(), sizeof(s_info.ap_ssid));

#if CONFIG_SEPLOS_QEMU_OPENETH
    (void)force_portal;
    return start_openeth();
#else
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, st.hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* credentials live in our own NVS namespace */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    const esp_timer_create_args_t targs = { .callback = reconnect_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    bool have_creds = st.wifi_ssid[0] != '\0';
    s_portal_forced = force_portal || !have_creds;
    /* In forced portal mode the station stays idle so the AP channel is stable;
     * the scan endpoint still works. */
    s_sta_enabled = have_creds && !force_portal;

    if (s_sta_enabled) {
        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, st.wifi_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, st.wifi_pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = st.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        sta.sta.pmf_cfg.capable = true;
        sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;      /* pick the strongest AP of a mesh */
        sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        strlcpy(s_info.ssid, st.wifi_ssid, sizeof(s_info.ssid));
        s_disconnected_since = esp_timer_get_time();
        ESP_LOGI(TAG, "connecting to '%s' as %s", st.wifi_ssid, st.hostname);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }
    if (s_portal_forced) {
        start_ap();
        if (have_creds) {
            const esp_timer_create_args_t pargs = { .callback = portal_timeout_cb, .name = "portal_to" };
            if (esp_timer_create(&pargs, &s_portal_timer) == ESP_OK) {
                esp_timer_start_once(s_portal_timer, PORTAL_IDLE_TIMEOUT_US);
            }
        }
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Stay responsive: modem sleep adds latency to the web UI on some routers. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
#endif
}

esp_err_t net_scan_json(sbuf_t *sb)
{
#if CONFIG_SEPLOS_QEMU_OPENETH
    sb_puts(sb, "[]");
    return ESP_OK;
#else
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        sb_puts(sb, "[]");
        return err;
    }
    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        sb_puts(sb, "[]");
        return ESP_ERR_NO_MEM;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    sb_putc(sb, '[');
    for (uint16_t i = 0; i < n; i++) {
        bool dup = false;
        for (uint16_t j = 0; j < i; j++) {
            dup |= strcmp((char *)recs[i].ssid, (char *)recs[j].ssid) == 0;
        }
        if (dup || recs[i].ssid[0] == '\0') {
            continue;
        }
        sb_puts(sb, "{\"ssid\":");
        sb_json_str(sb, (const char *)recs[i].ssid);
        sb_printf(sb, ",\"rssi\":%d,\"secure\":%s},", recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
    }
    sb_trim_comma(sb);
    sb_putc(sb, ']');
    free(recs);
    return ESP_OK;
#endif
}
