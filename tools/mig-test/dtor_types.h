/* SPDX-License-Identifier: GPL-3.0-only */
/* dtor_types.h - the translated type of dtor.defs (the host test dtor.c). */
#ifndef DTOR_TYPES_H
#define DTOR_TYPES_H
#include <stdint.h>
#include <mach/port.h>
typedef struct dtor_obj *dtor_obj_t;
dtor_obj_t dtor_in(mach_port_t name);
dtor_obj_t dtor_in_payload(uintptr_t payload);
void dtor_done(dtor_obj_t o);
#endif
