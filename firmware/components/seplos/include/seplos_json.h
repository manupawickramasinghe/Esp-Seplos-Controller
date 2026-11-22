/* JSON rendering of BMS state (pure C, host-testable). */
#pragma once

#include "sbuf.h"
#include "seplos_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t address;
    bool online;            /* last poll succeeded */
    bool has_data;          /* telemetry received at least once */
    bool has_alarms;        /* alarm frame received at least once */
    uint32_t age_ms;        /* time since last good telemetry */
    uint32_t ok_count;
    uint32_t err_count;
    seplos_err_t last_err;
    seplos_telemetry_t tele;
    seplos_alarms_t alarms;
} seplos_pack_state_t;

typedef enum {
    SEPLOS_TEMP_CELL,
    SEPLOS_TEMP_AMBIENT,
    SEPLOS_TEMP_POWER,
} seplos_temp_kind_t;

/* Seplos reports N cell sensors followed by ambient and power (MOS) sensors. */
seplos_temp_kind_t seplos_temp_kind(uint8_t idx, uint8_t temp_count);

/*
 * Original firmware compatible JSON (HTTP /AllJsonData and MQTT .../AllJsonData).
 * Same keys, order and decimal places as v1.0.1.2 so existing Grafana / Node-RED /
 * Home Assistant setups keep working.
 */
void seplos_json_legacy(sbuf_t *sb, const seplos_pack_state_t *pack, const char *bat_name);

/* New per-pack JSON object (used by /api/data and MQTT .../pack/<addr>/state). */
void seplos_json_pack(sbuf_t *sb, const seplos_pack_state_t *pack);

/* Pack power in 0.1 W (negative = discharge). */
int32_t seplos_power_dw(const seplos_telemetry_t *t);

#ifdef __cplusplus
}
#endif
