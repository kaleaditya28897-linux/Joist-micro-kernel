/* SPDX-License-Identifier: GPL-3.0-only */
/* notif_host.h - Joist's notifications emulated on the host (notif_host.c). */
#ifndef NOTIF_HOST_H
#define NOTIF_HOST_H

#include "joist-gsync.h"

#include <stdint.h>

#define NOTIF_MAX 256u

uint32_t notif_new(void);  /* a new notification, its bit clear */
void notif_reset(void);    /* forget them all (between tests) */
int notif_pending(uint32_t n); /* its bit */
unsigned notif_blocked(uint32_t n); /* threads blocked in Wait on it, not yet handed a signal */
extern unsigned long notif_signals, notif_waits, notif_timeouts;

#endif
