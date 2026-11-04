/* SPDX-License-Identifier: GPL-3.0-only */
/* fuzz_demux.c - libFuzzer over the demuxers MIG's Joist mode generates for
 * io, fs and fsys (Phase 5 step 6): any message a client could send (any
 * id, length, words, capabilities, badge) is either refused or served, and
 * never makes a demuxer read past what arrived or write past the reply.
 * The routines it reaches are the round-trip test's servers. Run under
 * ASan and UBSan. */
#include "jt.h"

#include <joist_mig.h>
#include <stdlib.h>
#include <string.h>

uint64_t jt_rng;
uint64_t jt_seen;
uint64_t jt_out_seed;

int io_server(struct joist_mig_msg *in, struct joist_mig_msg *out);
int fs_server(struct joist_mig_msg *in, struct joist_mig_msg *out);
int fsys_server(struct joist_mig_msg *in, struct joist_mig_msg *out);

int joist_mig_call(uint32_t port, struct joist_mig_msg *m)
{
    (void)port;
    (void)m;
    return 1; /* the servers make no calls of their own */
}

void *joist_mig_allocate(size_t bytes)
{
    return malloc(bytes != 0 ? bytes : 1u);
}

void joist_mig_deallocate(void *p, size_t bytes)
{
    (void)bytes;
    free(p);
}

void jt_own(void *pp, size_t n)
{
    *(void **)pp = joist_mig_allocate(n);
}

void jt_fail(const char *routine, int r, const char *what)
{
    (void)routine;
    (void)r;
    (void)what;
}

static uint64_t take(const uint8_t **p, size_t *n, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++) {
        v = v << 8 | (*n != 0 ? ((*n)--, *(*p)++) : 0u);
    }
    return v;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int (*const demux[])(struct joist_mig_msg *, struct joist_mig_msg *) = {io_server, fs_server,
                                                                                   fsys_server};
    /* The buffers as the kernel gives them: one page each, whatever len
     * says (a len past it is the kernel's to refuse; the demuxer must
     * refuse it too, never read beyond). */
    uint64_t inbuf[JOIST_MIG_BUF_WORDS];
    uint64_t outbuf[JOIST_MIG_BUF_WORDS];
    struct joist_mig_msg in;
    memset(&in, 0, sizeof in);
    memset(inbuf, 0, sizeof inbuf);
    unsigned which = (unsigned)take(&data, &size, 1) % 3u;
    /* Mostly a real routine's id, with a length that fits. */
    uint64_t sel = take(&data, &size, 1);
    uint32_t base = which == 0 ? 21000u : which == 1 ? 20000u : 22000u;
    uint32_t id = sel < 200u ? base + (uint32_t)(sel % 40u) : (uint32_t)take(&data, &size, 4);
    uint64_t bytes = take(&data, &size, 2);
    in.r[0] = id | (sel % 7u == 0 ? take(&data, &size, 4) << 32 : bytes % (JOIST_MIG_WORDS * 8u + 64u) << 32);
    for (int i = 1; i < 4; i++) {
        in.r[i] = take(&data, &size, 8);
    }
    in.ncaps = (uint32_t)take(&data, &size, 1) % 4u;
    for (int i = 0; i < 2; i++) {
        in.cap[i] = (uint32_t)take(&data, &size, 2);
    }
    in.badge = take(&data, &size, 4);
    uint64_t words = ((in.r[0] >> 32) + 7u) / 8u;
    in.len = take(&data, &size, 1) == 0 ? (uint32_t)take(&data, &size, 2) : (uint32_t)(words > 3u ? words - 3u : 0);
    size_t fill = size < sizeof inbuf ? size : sizeof inbuf;
    memcpy(inbuf, data, fill);
    in.buf = inbuf;
    jt_out_seed = in.badge;

    struct joist_mig_msg out;
    memset(&out, 0, sizeof out);
    out.buf = outbuf;
    (void)demux[which](&in, &out);
    if (out.len > JOIST_MIG_BUF_WORDS || out.ncaps > JOIST_MIG_CAPS) {
        abort(); /* a reply the kernel would refuse */
    }
    return 0;
}
