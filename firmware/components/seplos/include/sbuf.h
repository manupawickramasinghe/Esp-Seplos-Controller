/* Tiny bounded string builder used for JSON output (no heap, no ESP-IDF deps). */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} sbuf_t;

void sb_init(sbuf_t *sb, char *buf, size_t cap);
void sb_putc(sbuf_t *sb, char c);
void sb_puts(sbuf_t *sb, const char *s);
void sb_printf(sbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* JSON string literal with escaping, including the surrounding quotes. NULL -> "". */
void sb_json_str(sbuf_t *sb, const char *s);
/* Fixed-point integer `value` scaled by 10^decimals, printed with exactly `decimals` digits. */
void sb_fixed(sbuf_t *sb, int32_t value, unsigned decimals);
/* Re-scale: print value (in 10^-from_dec units) with `to_dec` digits, rounding half away from zero. */
void sb_fixed_as(sbuf_t *sb, int32_t value, unsigned from_dec, unsigned to_dec);
/* Remove a trailing comma if present (helper for list building). */
void sb_trim_comma(sbuf_t *sb);

#ifdef __cplusplus
}
#endif
