/* Renders the test frames as JSON; test/host/check_json.py validates the output. */
#include "seplos_json.h"

#include <stdio.h>
#include <string.h>

static const char TELEMETRY_FRAME[] =
    "~2000460010960000100D080D070D090D0A0D0D0D050D0A0D090D0D0D090D0D0D0C0D0C0D0B0D0C0D09060B8A0B890B840B86"
    "0BB50B99FF4514DD4C050A4E2003CD4E20005603E814E20000000000000000DD4D\r";
static const char ALARM_FRAME[] =
    "~20004600505600001000000000000000000000000000000000060000000000000000000001000000040320000100000000ED72\r";

int main(void)
{
    static char out[8192];
    seplos_frame_t f;
    seplos_pack_state_t p = { .address = 0, .online = true, .has_data = true, .has_alarms = true,
                              .age_ms = 250, .ok_count = 10, .err_count = 1 };
    if (seplos_parse_frame(TELEMETRY_FRAME, strlen(TELEMETRY_FRAME), &f) != SEPLOS_OK ||
        seplos_decode_telemetry(&f, &p.tele) != SEPLOS_OK ||
        seplos_parse_frame(ALARM_FRAME, strlen(ALARM_FRAME), &f) != SEPLOS_OK ||
        seplos_decode_alarms(&f, &p.alarms) != SEPLOS_OK) {
        fprintf(stderr, "decode failed\n");
        return 1;
    }
    sbuf_t sb;
    sb_init(&sb, out, sizeof out);
    seplos_json_legacy(&sb, &p, "Darth \"Vader\"\\ \x01");
    if (sb.overflow) return 2;
    printf("%s\n", out);

    sb_init(&sb, out, sizeof out);
    seplos_json_pack(&sb, &p);
    if (sb.overflow) return 3;
    printf("%s\n", out);

    /* offline pack without data */
    seplos_pack_state_t off = { .address = 2, .last_err = SEPLOS_ERR_NO_DATA };
    sb_init(&sb, out, sizeof out);
    seplos_json_pack(&sb, &off);
    printf("%s\n", out);
    sb_init(&sb, out, sizeof out);
    seplos_json_legacy(&sb, &off, "x");
    printf("%s\n", out);

    /* overflow handling: tiny buffer must not overrun and must flag overflow */
    char tiny[16];
    sb_init(&sb, tiny, sizeof tiny);
    seplos_json_pack(&sb, &p);
    if (!sb.overflow || strlen(tiny) >= sizeof tiny) return 4;

    /* fixed-point formatting edge cases */
    sb_init(&sb, out, sizeof out);
    sb_fixed(&sb, -5, 2); sb_putc(&sb, ' ');
    sb_fixed(&sb, 0, 3); sb_putc(&sb, ' ');
    sb_fixed_as(&sb, -187, 2, 1); sb_putc(&sb, ' ');
    sb_fixed_as(&sb, 973, 1, 2); sb_putc(&sb, ' ');
    sb_fixed(&sb, -2147483647 - 1, 2);
    printf("%s\n", out);
    return 0;
}
