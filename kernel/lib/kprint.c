/* SPDX-License-Identifier: GPL-3.0-only */
/* kprint.c - minimal kernel output to the serial console. See kprint.h. */
#include "joist/kprint.h"

#include "serial.h"

void kputs(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s);
    }
}

void kputu(uint64_t v)
{
    char buf[21]; /* 2^64 - 1 has 20 digits */
    unsigned n = 0;
    do {
        buf[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0 && n < sizeof buf);
    while (n > 0) {
        serial_putc(buf[--n]);
    }
}

void kputx(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    kputs("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_putc(digits[(v >> shift) & 0xfu]);
    }
}
