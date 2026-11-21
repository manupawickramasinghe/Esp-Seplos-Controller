/* MQTT publishing (original topic layout + Home Assistant discovery). */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t mqtt_start(void);
bool mqtt_is_enabled(void);
bool mqtt_is_connected(void);

/* Topic helpers for the UI / legacy dataonce endpoint. */
void mqtt_topic(char *out, size_t size, const char *suffix);

/* Publish a text message to <base>/Info (no-op when disconnected). */
void mqtt_info(const char *msg);
