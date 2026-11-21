#include "sbuf.h"

#include <stdio.h>
#include <string.h>

void sb_init(sbuf_t *sb, char *buf, size_t cap)
{
    sb->buf = buf;
    sb->cap = cap;
    sb->len = 0;
    sb->overflow = cap == 0;
    if (cap) {
        buf[0] = '\0';
    }
}

void sb_putc(sbuf_t *sb, char c)
{
    if (sb->len + 1 < sb->cap) {
        sb->buf[sb->len++] = c;
        sb->buf[sb->len] = '\0';
    } else {
        sb->overflow = true;
    }
}

void sb_puts(sbuf_t *sb, const char *s)
{
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    if (sb->len + n < sb->cap) {
        memcpy(sb->buf + sb->len, s, n + 1);
        sb->len += n;
    } else {
        sb->overflow = true;
    }
}

void sb_printf(sbuf_t *sb, const char *fmt, ...)
{
    if (sb->overflow || sb->len >= sb->cap) {
        sb->overflow = true;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sb->cap - sb->len) {
        sb->overflow = true;
        sb->buf[sb->len] = '\0';
        return;
    }
    sb->len += (size_t)n;
}

void sb_json_str(sbuf_t *sb, const char *s)
{
    sb_putc(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        default:
            if (*p < 0x20) {
                sb_printf(sb, "\\u%04x", *p);
            } else {
                sb_putc(sb, (char)*p);
            }
        }
    }
    sb_putc(sb, '"');
}

static const int32_t POW10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };

void sb_fixed(sbuf_t *sb, int32_t value, unsigned decimals)
{
    if (decimals > 6) {
        decimals = 6;
    }
    /* unsigned long arithmetic only: avoids %lld, which small libc printf variants may lack */
    const char *sign = value < 0 ? "-" : "";
    unsigned long v = value < 0 ? 0ul - (unsigned long)(uint32_t)value : (unsigned long)(uint32_t)value;
    v &= 0xFFFFFFFFul;
    if (decimals == 0) {
        sb_printf(sb, "%s%lu", sign, v);
        return;
    }
    unsigned long p = (unsigned long)POW10[decimals];
    sb_printf(sb, "%s%lu.%0*lu", sign, v / p, (int)decimals, v % p);
}

void sb_fixed_as(sbuf_t *sb, int32_t value, unsigned from_dec, unsigned to_dec)
{
    if (from_dec > 6) from_dec = 6;
    if (to_dec > 6) to_dec = 6;
    int64_t v = value;
    if (to_dec > from_dec) {
        v *= POW10[to_dec - from_dec];
    } else if (to_dec < from_dec) {
        int64_t d = POW10[from_dec - to_dec];
        v = (v >= 0) ? (v + d / 2) / d : -((-v + d / 2) / d);
    }
    sb_fixed(sb, (int32_t)v, to_dec);
}

void sb_trim_comma(sbuf_t *sb)
{
    if (sb->len && sb->buf[sb->len - 1] == ',') {
        sb->buf[--sb->len] = '\0';
    }
}
