/*
 * Seplos BMS RS485 protocol V2.0 (ASCII-hex framed, 19200 8N1).
 *
 * Pure C, no ESP-IDF dependencies, so it can be unit-tested on the host.
 *
 * Frame layout (every field is ASCII hex, two characters per byte):
 *
 *   '~' VER ADR CID1 CID2/RTN LENGTH INFO... CHKSUM '\r'
 *    1   2   2   2      2       4     LENID    4     1
 *
 *   LENGTH = LCHKSUM (4 bits) | LENID (12 bits, number of INFO characters)
 *   LCHKSUM = (~(sum of the three LENID nibbles) + 1) & 0xF
 *   CHKSUM  = (~(sum of all characters between '~' and CHKSUM) + 1) & 0xFFFF
 *
 * The field offsets and scale factors were recovered from the original
 * ESP8266 firmware v1.0.1.2 (see docs/reverse-engineering.md).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEPLOS_VER 0x20
#define SEPLOS_CID1_BATTERY 0x46

#define SEPLOS_CID2_TELEMETRY 0x42   /* analog values: cells, temps, current... */
#define SEPLOS_CID2_ALARMS 0x44      /* telecommand: alarm / status flags */
#define SEPLOS_CID2_CONTROL 0x45     /* telecontrol: used for BMS shutdown */

#define SEPLOS_MAX_CELLS 24
#define SEPLOS_MAX_TEMPS 8
#define SEPLOS_MAX_INFO_BYTES 256
/* Biggest frame we accept: header 13 + INFO + checksum 4 + EOI 1. */
#define SEPLOS_MAX_FRAME_CHARS (18 + 2 * SEPLOS_MAX_INFO_BYTES)

typedef enum {
    SEPLOS_OK = 0,
    SEPLOS_ERR_ARG,
    SEPLOS_ERR_NO_DATA,
    SEPLOS_ERR_TOO_SHORT,
    SEPLOS_ERR_SOI,
    SEPLOS_ERR_EOI,
    SEPLOS_ERR_HEX,
    SEPLOS_ERR_LENGTH_CHECKSUM,
    SEPLOS_ERR_LENGTH_MISMATCH,
    SEPLOS_ERR_CHECKSUM,
    SEPLOS_ERR_RTN,
    SEPLOS_ERR_PAYLOAD,
    SEPLOS_ERR_ADDRESS,
} seplos_err_t;

const char *seplos_err_str(seplos_err_t err);

/* Checksum over the ASCII characters between SOI and CHKSUM. */
uint16_t seplos_checksum(const char *ascii, size_t len);

/* Full 16-bit LENGTH field (LCHKSUM + LENID) for a given LENID. */
uint16_t seplos_length_field(uint16_t lenid);

/*
 * Build a request frame into `out` (NUL terminated). Returns the frame length
 * in characters (without the NUL) or 0 if `out` is too small.
 */
size_t seplos_build_frame(char *out, size_t out_size, uint8_t addr, uint8_t cid1, uint8_t cid2,
                          const uint8_t *info, size_t info_len);

/* 0x42 telemetry request, INFO = pack address. Addr 0 -> "~20004642E00200FD37\r". */
size_t seplos_build_telemetry_request(char *out, size_t out_size, uint8_t addr);

/* 0x44 alarm/status request, INFO = pack address. */
size_t seplos_build_alarm_request(char *out, size_t out_size, uint8_t addr);

/*
 * 0x45 shutdown request. For address 0 this is byte-identical to the frame the
 * original firmware sent ("~20004645A006000400FC70\r"). For other addresses the
 * ADR field and the first INFO byte carry the pack address.
 */
size_t seplos_build_shutdown_request(char *out, size_t out_size, uint8_t addr);

typedef struct {
    uint8_t ver;
    uint8_t addr;
    uint8_t cid1;
    uint8_t rtn; /* CID2 position: 0x00 = OK in responses */
    uint16_t info_len; /* bytes */
    uint8_t info[SEPLOS_MAX_INFO_BYTES];
} seplos_frame_t;

/*
 * Validate and decode a complete response frame ('~' ... '\r').
 * Checks SOI, EOI, hex, LENGTH checksum, actual length and CHKSUM.
 * A non-zero RTN is reported as SEPLOS_ERR_RTN (frame still decoded).
 */
seplos_err_t seplos_parse_frame(const char *ascii, size_t len, seplos_frame_t *out);

/* Telemetry (CID2 0x42) in the BMS's native integer units. */
typedef struct {
    uint8_t data_flag;
    uint8_t pack;               /* command value / pack number echoed back */
    uint8_t cell_count;
    uint16_t cell_mv[SEPLOS_MAX_CELLS];
    uint8_t temp_count;          /* usually 6: cell 1..4, ambient, power(MOS) */
    int16_t temp_dc[SEPLOS_MAX_TEMPS]; /* 0.1 degC */
    int16_t current_ca;          /* 0.01 A, negative = discharge */
    uint16_t voltage_cv;         /* 0.01 V pack voltage */
    uint16_t remain_cah;         /* 0.01 Ah remaining capacity */
    uint8_t custom_count;        /* "P", usually 10 */
    uint16_t capacity_cah;       /* 0.01 Ah full capacity */
    uint16_t soc_pm;             /* 0.1 % state of charge */
    uint16_t rated_cah;          /* 0.01 Ah rated capacity */
    uint16_t cycles;
    uint16_t soh_pm;             /* 0.1 % state of health */
    uint16_t port_voltage_cv;    /* 0.01 V bus / port voltage */
    uint16_t reserved[4];

    /* derived */
    uint16_t cell_min_mv;
    uint16_t cell_max_mv;
    uint8_t cell_min_idx;        /* 0 based */
    uint8_t cell_max_idx;        /* 0 based */
    uint16_t cell_diff_mv;
} seplos_telemetry_t;

seplos_err_t seplos_decode_telemetry(const seplos_frame_t *frame, seplos_telemetry_t *out);

/* Alarm / status (CID2 0x44). Layout per Seplos protocol V2.0. */
typedef struct {
    uint8_t data_flag;
    uint8_t pack;
    uint8_t cell_count;
    uint8_t cell_alarm[SEPLOS_MAX_CELLS]; /* 0 ok, 1 below lower limit, 2 above upper limit, 0xF0 other */
    uint8_t temp_count;
    uint8_t temp_alarm[SEPLOS_MAX_TEMPS];
    uint8_t charge_current_alarm;
    uint8_t total_voltage_alarm;
    uint8_t discharge_current_alarm;
    uint8_t event[8];            /* alarm events 1..8 (bit fields) */
    uint8_t switch_state;        /* bit0 discharge, bit1 charge, bit2 current limit, bit3 heating */
    uint16_t balancing;          /* bit n = cell n+1 balancing */
    uint8_t system_state;        /* bit0 discharge, bit1 charge, bit2 float, bit4 standby, bit5 shutdown */
    uint16_t disconnected;       /* bit n = cell n+1 disconnected */
} seplos_alarms_t;

seplos_err_t seplos_decode_alarms(const seplos_frame_t *frame, seplos_alarms_t *out);

/* Human readable alarm text for event byte `event_idx` (0..7) bit `bit`, or NULL if reserved. */
const char *seplos_alarm_event_text(unsigned event_idx, unsigned bit);

/* True if the bit is a protection/fault (as opposed to a warning or status bit). */
bool seplos_alarm_event_is_protection(unsigned event_idx, unsigned bit);

/* Number of active (non-reserved) alarm event bits plus non-zero per-cell/temp alarms. */
unsigned seplos_alarm_count(const seplos_alarms_t *alarms);

#ifdef __cplusplus
}
#endif
