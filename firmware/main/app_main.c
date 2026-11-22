/*
 * Seplos BMS controller (ESP-IDF).
 *
 * Re-implementation of the closed-source ESP8266 "ESP-Seplos-Controller" v1.0.1.2:
 * reads a Seplos BMS over RS485 (protocol V2.0), serves a web UI + JSON API and
 * publishes to MQTT (same topics/payloads as the original, plus Home Assistant
 * discovery). See README.md for wiring and docs/reverse-engineering.md for the
 * analysis of the original binary.
 */
#include <stdlib.h>
#include <time.h>

#include "bms.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt.h"
#include "net.h"
#include "nvs_flash.h"
#include "settings.h"
#include "status_led.h"
#include "web.h"

static const char *TAG = "main";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated or has a new format, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* Confirm a freshly installed OTA image once the system has run stably for a while,
 * otherwise the bootloader rolls back to the previous firmware on the next reset. */
static void confirm_ota_cb(void *arg)
{
    (void)arg;
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Seplos Controller %s (ESP-IDF %s)", app->version, app->idf_ver);

    init_nvs();
    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    app_settings_t st;
    settings_get(&st);
    setenv("TZ", st.tz, 1);
    tzset();

    bool portal = settings_take_portal_request();
    if (settings_double_reset_detected()) {
        portal = true;
    }
    if (button_is_pressed()) {
        ESP_LOGW(TAG, "setup button held at boot -> setup portal");
        portal = true;
    }
    ESP_LOGI(TAG, "controller id %s", settings_controller_id());

    ESP_ERROR_CHECK(bms_start(st.first_addr, st.pack_count, st.poll_ms));
    ESP_ERROR_CHECK(net_start(portal));
    ESP_ERROR_CHECK(web_start());
    ESP_ERROR_CHECK(mqtt_start());
    status_led_start();

    static esp_timer_handle_t confirm;
    const esp_timer_create_args_t a = { .callback = confirm_ota_cb, .name = "ota_ok" };
    if (esp_timer_create(&a, &confirm) == ESP_OK) {
        esp_timer_start_once(confirm, 30 * 1000 * 1000);
    }
}
