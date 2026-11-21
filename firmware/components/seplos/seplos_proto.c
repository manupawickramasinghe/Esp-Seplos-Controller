#include "seplos_proto.h"

#include <string.h>

static const char HEX[] = "0123456789ABCDEF";

const char *seplos_err_str(seplos_err_t err)
{
    switch (err) {
    case SEPLOS_OK: return "ok";
    case SEPLOS_ERR_ARG: return "invalid argument";
    case SEPLOS_ERR_NO_DATA: return "no response from BMS";
    case SEPLOS_ERR_TOO_SHORT: return "frame too short";
    case SEPLOS_ERR_SOI: return "missing start '~'";
    case SEPLOS_ERR_EOI: return "missing end <CR>";
    case SEPLOS_ERR_HEX: return "invalid hex character";
    case SEPLOS_ERR_LENGTH_CHECKSUM: return "length checksum wrong";
    case SEPLOS_ERR_LENGTH_MISMATCH: return "length does not match frame";
    case SEPLOS_ERR_CHECKSUM: return "frame checksum wrong";
    case SEPLOS_ERR_RTN: return "BMS returned error code";
    case SEPLOS_ERR_PAYLOAD: return "payload shorter than declared";
    case SEPLOS_ERR_ADDRESS: return "response from unexpected address";
    }
    return "unknown error";
}

uint16_t seplos_checksum(const char *ascii, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += (uint8_t)ascii[i];
    }
    return (uint16_t)((~sum + 1u) & 0xFFFFu);
}

uint16_t seplos_length_field(uint16_t lenid)
{
    lenid &= 0x0FFF;
    unsigned nibble_sum = (lenid & 0xF) + ((lenid >> 4) & 0xF) + ((lenid >> 8) & 0xF);
    unsigned lchk = (~nibble_sum + 1u) & 0xFu;
    return (uint16_t)((lchk << 12) | lenid);
}

static void put_hex8(char *p, uint8_t v)
{
    p[0] = HEX[v >> 4];
    p[1] = HEX[v & 0xF];
}

static void put_hex16(char *p, uint16_t v)
{
    put_hex8(p, (uint8_t)(v >> 8));
    put_hex8(p + 2, (uint8_t)v);
}

size_t seplos_build_frame(char *out, size_t out_size, uint8_t addr, uint8_t cid1, uint8_t cid2,
                          const uint8_t *info, size_t info_len)
{
    if (!out || (info_len && !info) || info_len > SEPLOS_MAX_INFO_BYTES) {
        return 0;
    }
    size_t lenid = info_len * 2;
    size_t total = 1 + 12 + lenid + 4 + 1;
    if (out_size < total + 1) {
        return 0;
    }
    char *p = out;
    *p++ = '~';
    put_hex8(p, SEPLOS_VER); p += 2;
    put_hex8(p, addr); p += 2;
    put_hex8(p, cid1); p += 2;
    put_hex8(p, cid2); p += 2;
    put_hex16(p, seplos_length_field((uint16_t)lenid)); p += 4;
    for (size_t i = 0; i < info_len; i++) {
        put_hex8(p, info[i]);
        p += 2;
    }
    put_hex16(p, seplos_checksum(out + 1, (size_t)(p - out - 1))); p += 4;
    *p++ = '\r';
    *p = '\0';
    return total;
}

size_t seplos_build_telemetry_request(char *out, size_t out_size, uint8_t addr)
{
    return seplos_build_frame(out, out_size, addr, SEPLOS_CID1_BATTERY, SEPLOS_CID2_TELEMETRY, &addr, 1);
}

size_t seplos_build_alarm_request(char *out, size_t out_size, uint8_t addr)
{
    return seplos_build_frame(out, out_size, addr, SEPLOS_CID1_BATTERY, SEPLOS_CID2_ALARMS, &addr, 1);
}

size_t seplos_build_shutdown_request(char *out, size_t out_size, uint8_t addr)
{
    /* INFO taken from the original firmware: 00 04 00 (pack, command "shutdown", 00). */
    const uint8_t info[3] = { addr, 0x04, 0x00 };
    return seplos_build_frame(out, out_size, addr, SEPLOS_CID1_BATTERY, SEPLOS_CID2_CONTROL, info, sizeof(info));
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool get_hex(const char *p, size_t nchars, uint32_t *out)
{
    uint32_t v = 0;
    for (size_t i = 0; i < nchars; i++) {
        int h = hexval(p[i]);
        if (h < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return true;
}

seplos_err_t seplos_parse_frame(const char *ascii, size_t len, seplos_frame_t *out)
{
    if (!ascii || !out) {
        return SEPLOS_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (len == 0) {
        return SEPLOS_ERR_NO_DATA;
    }
    /* Skip leading noise (line idle glitches after the transceiver turns around). */
    const char *soi = memchr(ascii, '~', len);
    if (!soi) {
        return SEPLOS_ERR_SOI;
    }
    len -= (size_t)(soi - ascii);
    ascii = soi;

    if (len < 18) {
        return SEPLOS_ERR_TOO_SHORT;
    }

    uint32_t ver, addr, cid1, rtn, length;
    if (!get_hex(ascii + 1, 2, &ver) || !get_hex(ascii + 3, 2, &addr) || !get_hex(ascii + 5, 2, &cid1) ||
        !get_hex(ascii + 7, 2, &rtn) || !get_hex(ascii + 9, 4, &length)) {
        return SEPLOS_ERR_HEX;
    }
    uint16_t lenid = (uint16_t)(length & 0x0FFF);
    if (seplos_length_field(lenid) != (uint16_t)length) {
        return SEPLOS_ERR_LENGTH_CHECKSUM;
    }
    if ((lenid & 1u) || lenid / 2 > SEPLOS_MAX_INFO_BYTES) {
        return SEPLOS_ERR_LENGTH_MISMATCH;
    }
    size_t expected = 18u + lenid;
    if (len < expected) {
        return SEPLOS_ERR_TOO_SHORT;
    }
    if (ascii[expected - 1] != '\r') {
        return SEPLOS_ERR_EOI;
    }

    uint32_t chk;
    if (!get_hex(ascii + 13 + lenid, 4, &chk)) {
        return SEPLOS_ERR_HEX;
    }
    if (seplos_checksum(ascii + 1, 12u + lenid) != (uint16_t)chk) {
        return SEPLOS_ERR_CHECKSUM;
    }

    out->ver = (uint8_t)ver;
    out->addr = (uint8_t)addr;
    out->cid1 = (uint8_t)cid1;
    out->rtn = (uint8_t)rtn;
    out->info_len = lenid / 2;
    for (size_t i = 0; i < out->info_len; i++) {
        uint32_t b;
        if (!get_hex(ascii + 13 + 2 * i, 2, &b)) {
            return SEPLOS_ERR_HEX;
        }
        out->info[i] = (uint8_t)b;
    }
    return out->rtn == 0 ? SEPLOS_OK : SEPLOS_ERR_RTN;
}

/* Bounded big-endian reader over INFO. */
typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
    bool overrun;
} reader_t;

static uint8_t rd8(reader_t *r)
{
    if (r->pos + 1 > r->len) {
        r->overrun = true;
        return 0;
    }
    return r->p[r->pos++];
}

static uint16_t rd16(reader_t *r)
{
    if (r->pos + 2 > r->len) {
        r->overrun = true;
        r->pos = r->len;
        return 0;
    }
    uint16_t v = (uint16_t)((r->p[r->pos] << 8) | r->p[r->pos + 1]);
    r->pos += 2;
    return v;
}

seplos_err_t seplos_decode_telemetry(const seplos_frame_t *frame, seplos_telemetry_t *t)
{
    if (!frame || !t) {
        return SEPLOS_ERR_ARG;
    }
    memset(t, 0, sizeof(*t));
    reader_t r = { .p = frame->info, .len = frame->info_len };

    t->data_flag = rd8(&r);
    t->pack = rd8(&r);
    t->cell_count = rd8(&r);
    if (t->cell_count > SEPLOS_MAX_CELLS) {
        return SEPLOS_ERR_PAYLOAD;
    }
    t->cell_min_mv = 0xFFFF;
    for (uint8_t i = 0; i < t->cell_count; i++) {
        uint16_t mv = rd16(&r);
        t->cell_mv[i] = mv;
        if (mv < t->cell_min_mv) {
            t->cell_min_mv = mv;
            t->cell_min_idx = i;
        }
        if (mv > t->cell_max_mv) {
            t->cell_max_mv = mv;
            t->cell_max_idx = i;
        }
    }
    if (t->cell_count == 0) {
        t->cell_min_mv = 0;
    }
    t->cell_diff_mv = (uint16_t)(t->cell_max_mv - t->cell_min_mv);

    t->temp_count = rd8(&r);
    if (t->temp_count > SEPLOS_MAX_TEMPS) {
        return SEPLOS_ERR_PAYLOAD;
    }
    for (uint8_t i = 0; i < t->temp_count; i++) {
        /* Kelvin * 10 -> degC * 10 */
        t->temp_dc[i] = (int16_t)((int32_t)rd16(&r) - 2731);
    }
    t->current_ca = (int16_t)rd16(&r);
    t->voltage_cv = rd16(&r);
    t->remain_cah = rd16(&r);
    t->custom_count = rd8(&r);
    t->capacity_cah = rd16(&r);
    t->soc_pm = rd16(&r);
    t->rated_cah = rd16(&r);
    t->cycles = rd16(&r);
    t->soh_pm = rd16(&r);
    t->port_voltage_cv = rd16(&r);
    if (r.overrun) {
        return SEPLOS_ERR_PAYLOAD;
    }
    /* Reserved words are optional; read what is there. */
    for (int i = 0; i < 4 && r.pos + 2 <= r.len; i++) {
        t->reserved[i] = rd16(&r);
    }
    return SEPLOS_OK;
}

seplos_err_t seplos_decode_alarms(const seplos_frame_t *frame, seplos_alarms_t *a)
{
    if (!frame || !a) {
        return SEPLOS_ERR_ARG;
    }
    memset(a, 0, sizeof(*a));
    reader_t r = { .p = frame->info, .len = frame->info_len };

    a->data_flag = rd8(&r);
    a->pack = rd8(&r);
    a->cell_count = rd8(&r);
    if (a->cell_count > SEPLOS_MAX_CELLS) {
        return SEPLOS_ERR_PAYLOAD;
    }
    for (uint8_t i = 0; i < a->cell_count; i++) {
        a->cell_alarm[i] = rd8(&r);
    }
    a->temp_count = rd8(&r);
    if (a->temp_count > SEPLOS_MAX_TEMPS) {
        return SEPLOS_ERR_PAYLOAD;
    }
    for (uint8_t i = 0; i < a->temp_count; i++) {
        a->temp_alarm[i] = rd8(&r);
    }
    a->charge_current_alarm = rd8(&r);
    a->total_voltage_alarm = rd8(&r);
    a->discharge_current_alarm = rd8(&r);
    for (int i = 0; i < 6; i++) {
        a->event[i] = rd8(&r);
    }
    a->switch_state = rd8(&r);
    uint8_t bal_lo = rd8(&r); /* cells 1..8 */
    uint8_t bal_hi = rd8(&r); /* cells 9..16 */
    a->balancing = (uint16_t)(bal_lo | (bal_hi << 8));
    a->system_state = rd8(&r);
    uint8_t dis_lo = rd8(&r);
    uint8_t dis_hi = rd8(&r);
    a->disconnected = (uint16_t)(dis_lo | (dis_hi << 8));
    if (r.overrun) {
        return SEPLOS_ERR_PAYLOAD;
    }
    /* Events 7 and 8 were added later in the protocol; treat as optional. */
    if (r.pos < r.len) {
        a->event[6] = rd8(&r);
    }
    if (r.pos < r.len) {
        a->event[7] = rd8(&r);
    }
    return SEPLOS_OK;
}

typedef struct {
    const char *text;
    bool protection;
} alarm_bit_t;

/* Seplos protocol V2.0, "alarm event 1..8". NULL = reserved. */
static const alarm_bit_t ALARM_BITS[8][8] = {
    { /* event 1: hardware faults */
        { "Voltage sensor fault", true }, { "Temperature sensor fault", true },
        { "Current sensor fault", true }, { "Key switch fault", true },
        { "Cell voltage difference fault", true }, { "Charge switch fault", true },
        { "Discharge switch fault", true }, { "Current limit switch fault", true },
    },
    { /* event 2: voltages */
        { "Cell high voltage alarm", false }, { "Cell over-voltage protection", true },
        { "Cell low voltage alarm", false }, { "Cell under-voltage protection", true },
        { "Pack high voltage alarm", false }, { "Pack over-voltage protection", true },
        { "Pack low voltage alarm", false }, { "Pack under-voltage protection", true },
    },
    { /* event 3: cell temperatures */
        { "Charge high temperature alarm", false }, { "Charge over-temperature protection", true },
        { "Charge low temperature alarm", false }, { "Charge under-temperature protection", true },
        { "Discharge high temperature alarm", false }, { "Discharge over-temperature protection", true },
        { "Discharge low temperature alarm", false }, { "Discharge under-temperature protection", true },
    },
    { /* event 4: ambient / power temperatures */
        { "Ambient high temperature alarm", false }, { "Ambient over-temperature protection", true },
        { "Ambient low temperature alarm", false }, { "Ambient under-temperature protection", true },
        { "Power (MOS) over-temperature protection", true }, { "Power (MOS) high temperature alarm", false },
        { "Cell low temperature heating", false }, { NULL, false },
    },
    { /* event 5: currents */
        { "Charge over-current alarm", false }, { "Charge over-current protection", true },
        { "Discharge over-current alarm", false }, { "Discharge over-current protection", true },
        { "Transient over-current protection", true }, { "Output short-circuit protection", true },
        { "Transient over-current lockout", true }, { "Output short-circuit lockout", true },
    },
    { /* event 6: misc */
        { "Charge high voltage protection", true }, { "Intermittent recharge waiting", false },
        { "Residual capacity alarm", false }, { "Residual capacity protection", true },
        { "Cell low voltage, charging forbidden", true }, { "Output reverse polarity protection", true },
        { "Output connection fault", true }, { NULL, false },
    },
    { /* event 7 */
        { NULL, false }, { NULL, false }, { NULL, false }, { NULL, false },
        { "Automatic charging waiting", false }, { "Manual charging waiting", false },
        { NULL, false }, { NULL, false },
    },
    { /* event 8: internal */
        { "EEPROM storage fault", true }, { "RTC clock fault", true },
        { "Voltage not calibrated", false }, { "Current not calibrated", false },
        { "Zero point not calibrated", false }, { NULL, false }, { NULL, false }, { NULL, false },
    },
};

const char *seplos_alarm_event_text(unsigned event_idx, unsigned bit)
{
    if (event_idx >= 8 || bit >= 8) {
        return NULL;
    }
    return ALARM_BITS[event_idx][bit].text;
}

bool seplos_alarm_event_is_protection(unsigned event_idx, unsigned bit)
{
    if (event_idx >= 8 || bit >= 8) {
        return false;
    }
    return ALARM_BITS[event_idx][bit].protection;
}

unsigned seplos_alarm_count(const seplos_alarms_t *a)
{
    if (!a) {
        return 0;
    }
    unsigned n = 0;
    for (unsigned e = 0; e < 8; e++) {
        for (unsigned b = 0; b < 8; b++) {
            if ((a->event[e] & (1u << b)) && ALARM_BITS[e][b].text) {
                n++;
            }
        }
    }
    for (unsigned i = 0; i < a->cell_count && i < SEPLOS_MAX_CELLS; i++) {
        n += a->cell_alarm[i] != 0;
    }
    for (unsigned i = 0; i < a->temp_count && i < SEPLOS_MAX_TEMPS; i++) {
        n += a->temp_alarm[i] != 0;
    }
    n += a->charge_current_alarm != 0;
    n += a->total_voltage_alarm != 0;
    n += a->discharge_current_alarm != 0;
    return n;
}
