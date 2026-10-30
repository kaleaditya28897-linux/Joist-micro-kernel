/* SPDX-License-Identifier: GPL-3.0-only */
/* serial.h - the 16550 UART on COM1, polled (Phase 2 serial console). */
#ifndef JOIST_ARCH_SERIAL_H
#define JOIST_ARCH_SERIAL_H

void serial_init(void);
void serial_putc(char c);

#endif /* JOIST_ARCH_SERIAL_H */
