/* RS485 polling of one or more Seplos BMS packs. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "seplos_json.h"

#define BMS_MAX_PACKS 16

esp_err_t bms_start(uint8_t first_addr, uint8_t pack_count, uint16_t poll_ms);

/* Copy the current state of all packs; returns the number of packs. */
size_t bms_snapshot(seplos_pack_state_t *out, size_t max);

/* Blocks until a new poll cycle completed or timeout; returns the cycle counter. */
uint32_t bms_wait_cycle(uint32_t last_seen, uint32_t timeout_ms);

/* Queue a shutdown frame (CID2 0x45) for the given pack address. */
esp_err_t bms_request_shutdown(uint8_t addr);

/* Human readable result of the last shutdown request ("" if none). */
void bms_last_shutdown_result(char *out, size_t size);

/* True if at least one pack answered in the last cycle. */
bool bms_any_online(void);
