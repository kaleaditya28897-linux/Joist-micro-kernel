/* SPDX-License-Identifier: GPL-3.0-only */
/* lpt_types.h - lpt.defs's translations (tst-libports.c defines them). */
#ifndef LPT_TYPES_H
#define LPT_TYPES_H 1
#include <mach.h>

typedef struct lpt_obj *lpt_obj_t;
lpt_obj_t lpt_in (mach_port_t name);
lpt_obj_t lpt_in_payload (unsigned long payload);
void lpt_done (lpt_obj_t obj);

#endif
