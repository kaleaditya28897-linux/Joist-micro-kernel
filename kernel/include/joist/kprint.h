/* SPDX-License-Identifier: GPL-3.0-only */
/* kprint.h - minimal kernel output (Phase 2). No printf: three helpers whose
 * output is easy to grep in the serial log and that can't overflow. */
#ifndef JOIST_KPRINT_H
#define JOIST_KPRINT_H

#include <stdint.h>

void kputs(const char *s); /* the string, as is */
void kputu(uint64_t v);    /* decimal */
void kputx(uint64_t v);    /* 0x-prefixed hexadecimal, 16 digits */

#endif /* JOIST_KPRINT_H */
