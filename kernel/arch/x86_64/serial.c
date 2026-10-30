/* SPDX-License-Identifier: GPL-3.0-only */
/* serial.c - the 16550 UART on COM1 (0x3f8), polled, 115200 8N1. Polled so it
 * works before interrupts exist and can't lose output to a full buffer. */
#include "serial.h"

#include "io.h"

#define COM1 0x3f8u

void serial_init(void)
{
    outb(COM1 + 1u, 0x00); /* no interrupts */
    outb(COM1 + 3u, 0x80); /* DLAB on: set the divisor */
    outb(COM1 + 0u, 0x01); /* 115200 baud */
    outb(COM1 + 1u, 0x00);
    outb(COM1 + 3u, 0x03); /* 8 bits, no parity, one stop bit; DLAB off */
    outb(COM1 + 2u, 0xc7); /* FIFO on, cleared, 14-byte threshold */
    outb(COM1 + 4u, 0x03); /* DTR, RTS */
}

void serial_putc(char c)
{
    /* Wait for the transmit holding register. Bounded on real UARTs and in
     * QEMU; a missing UART reads 0xff, which also reads as "empty". */
    for (unsigned spin = 0; spin < 100000u && (inb(COM1 + 5u) & 0x20u) == 0; spin++) {
    }
    outb(COM1, (uint8_t)c);
}
