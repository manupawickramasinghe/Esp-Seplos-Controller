/* Wi-Fi station + setup access point (captive portal), mDNS and SNTP. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "sbuf.h"

typedef struct {
    bool connected;      /* station has an IP (or Ethernet in QEMU) */
    bool portal;         /* setup access point is running */
    char ssid[33];
    int8_t rssi;
    char ip[16];
    char ap_ssid[33];
    char hostname[33];
} net_info_t;

esp_err_t net_start(bool force_portal);
void net_get_info(net_info_t *out);
bool net_is_connected(void);

/* Blocking Wi-Fi scan, writes a JSON array of {ssid,rssi,secure}. */
esp_err_t net_scan_json(sbuf_t *sb);

/* Wall clock time when the firmware started (0 until SNTP synced). */
time_t net_boot_time(void);

/* Called when the station got an IP address. */
typedef void (*net_ip_cb_t)(void);
void net_on_connected(net_ip_cb_t cb);
