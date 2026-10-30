/* SPDX-License-Identifier: GPL-3.0-only */
/* kerr.h - kernel-internal result codes (Phase 2). Not the system-call ABI's
 * codes (docs/abi.md), which Phase 3 brings in. */
#ifndef JOIST_KERR_H
#define JOIST_KERR_H

typedef enum {
    KERR_OK = 0,
    KERR_INVAL, /* malformed input: a bad memory map, a misaligned address */
    KERR_NOMEM, /* nothing left to allocate */
    KERR_RANGE, /* an address outside what the structure covers */
    KERR_STATE  /* the operation contradicts the current state: freeing a free frame */
} kerr;

#endif /* JOIST_KERR_H */
