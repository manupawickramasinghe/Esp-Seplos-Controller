/* Persistent settings (NVS namespace "seplos"). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SETTINGS_NAME_LEN 33

typedef struct {
    char bat_name[SETTINGS_NAME_LEN];
    char hostname[33];
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_uri[129];      /* mqtt://host:1883 or mqtts://host:8883, empty = MQTT off */
    char mqtt_user[65];
    char mqtt_pass[65];
    char mqtt_base[65];      /* base topic, default Seplos/<controller id> */
    bool mqtt_ha_discovery;
    bool allow_remote_shutdown;
    char admin_pass[65];     /* empty = no password for settings / OTA / shutdown */
    char tz[65];             /* POSIX TZ string */
    uint8_t first_addr;
    uint8_t pack_count;
    uint16_t poll_ms;
} app_settings_t;

/* Controller id "SEPLOS_<chip id>", same format as the original firmware. */
const char *settings_controller_id(void);

esp_err_t settings_init(void);

/* Thread-safe copy of the current settings. */
void settings_get(app_settings_t *out);

/* Validate + persist. Returns ESP_ERR_INVALID_ARG with a message on bad input. */
esp_err_t settings_save(const app_settings_t *s, const char **error_msg);

/* Battery name only (no restart needed). */
esp_err_t settings_set_name(const char *name);
void settings_get_name(char *out, unsigned size);

/* One-shot boot flags stored in NVS. */
bool settings_take_portal_request(void);
void settings_request_portal(void);
/* Double reset detection: returns true when the board was reset twice within 10 s. */
bool settings_double_reset_detected(void);
