/* SPDX-License-Identifier: GPL-3.0-only */
/* jt.h - what the round-trip tests MIG's Joist mode writes (-test) share
 * (Phase 5 step 6): a hash, a seeded stream, and the record of what a test
 * server saw. One copy of the state, in transport.c. */
#ifndef JOIST_MIG_JT_H
#define JOIST_MIG_JT_H

#include <stddef.h>
#include <stdint.h>

#define JT_H0 0xcbf29ce484222325ull /* FNV-1a */

extern uint64_t jt_rng;      /* the stream's state */
extern uint64_t jt_seen;     /* the hash the test server took of what it got */
extern uint64_t jt_out_seed; /* where the test server's outputs come from */

void jt_fail(const char *routine, int r, const char *what);
/* After a client stub returns: both its thread's windows clear (0032). */
void jt_check_windows(const char *routine);
/* *p made the server's own memory of n bytes (an out array it moves). */
void jt_own(void *pp, size_t n);

static inline uint64_t jt_mix(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

static inline uint64_t jt_next(void)
{
    jt_rng ^= jt_rng << 13;
    jt_rng ^= jt_rng >> 7;
    jt_rng ^= jt_rng << 17;
    return jt_rng;
}

static inline void jt_fill(void *p, size_t n)
{
    unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        b[i] = (unsigned char)jt_next();
    }
}

/* The largest array a round trip sends (0032's windows: past the in-line
 * limit, about 4 KiB, and the kernel's copy limit, 64 KiB, twice). */
#define JT_BIG (160u * 1024u)

/* A size in bytes, at most max: often small, often about the in-line limit
 * (4 120 bytes a body) or the kernel's copy limit (65 536 a call), sometimes
 * past both. */
static inline size_t jt_size(size_t max)
{
    size_t n;
    switch (jt_next() % 8u) {
    case 0:
    case 1:
    case 2:
        n = jt_next() % 301u;
        break;
    case 3:
    case 4:
        n = 3900u + jt_next() % 400u;
        break;
    case 5:
        n = 65536u - 200u + jt_next() % 400u;
        break;
    case 6:
        n = 131072u - 100u + jt_next() % 200u;
        break;
    default:
        n = jt_next() % (JT_BIG + 1u);
        break;
    }
    return n < max ? n : max;
}

/* A string of up to cap - 1 letters, terminated. */
static inline void jt_string(char *s, size_t cap)
{
    size_t n = cap > 1u ? jt_next() % cap : 0;
    for (size_t i = 0; i < n; i++) {
        s[i] = (char)('a' + jt_next() % 26u);
    }
    s[n] = 0;
}

#endif
