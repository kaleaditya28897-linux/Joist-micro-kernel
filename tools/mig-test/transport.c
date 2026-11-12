/* SPDX-License-Identifier: GPL-3.0-only */
/* transport.c - the round-trip test's fake transport and main (Phase 5 step
 * 6): joist_mig_call() does what the kernel's Call does to a message
 * (registers, the IPC buffer copied whole, capabilities moved to other
 * slots: +1000 on the way in, +2000 on the way back) and hands it to the
 * interfaces' demuxers; the runtime's memory is malloc. Then every carried
 * routine of io, fs and fsys, once per seed, and a few messages spoilt on
 * the way (jt_tamper) that the demuxers must refuse. */
#include "jt.h"

#include <hurd/hurd_types.h>
#include <joist_mig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t jt_rng;
uint64_t jt_seen;
uint64_t jt_out_seed;
static unsigned failed;

/* How the transport spoils the next request (0: it doesn't). */
enum { T_NONE, T_EXTRA, T_LEN, T_ID, T_NOCAP, T_NOREPLYCAP, T_NONUL };
static int jt_tamper;

int io_server(struct joist_mig_msg *in, struct joist_mig_msg *out);
int fs_server(struct joist_mig_msg *in, struct joist_mig_msg *out);
int fsys_server(struct joist_mig_msg *in, struct joist_mig_msg *out);
int jt_io(uint64_t seed, unsigned *calls);
int jt_fs(uint64_t seed, unsigned *calls);
int jt_fsys(uint64_t seed, unsigned *calls);

static int (*const demux[])(struct joist_mig_msg *, struct joist_mig_msg *) = {io_server, fs_server,
                                                                               fsys_server};
joist_mig_routine_t io_server_routine(const struct joist_mig_msg *in);
joist_mig_routine_t fs_server_routine(const struct joist_mig_msg *in);
joist_mig_routine_t fsys_server_routine(const struct joist_mig_msg *in);
/* MIG's other entry (<demuxer>_routine: the routine for an id, or none),
 * taken on every other call that isn't spoilt, so both are checked by the
 * same calls. */
static joist_mig_routine_t (*const lookup[])(const struct joist_mig_msg *) = {
    io_server_routine, fs_server_routine, fsys_server_routine};
static unsigned long jt_calls;

int joist_mig_call(uint32_t port, struct joist_mig_msg *m)
{
    if (m->len > JOIST_MIG_BUF_WORDS || m->ncaps > JOIST_MIG_CAPS) {
        return 1; /* the kernel's E_RANGE */
    }
    uint64_t inbuf[JOIST_MIG_BUF_WORDS];
    uint64_t outbuf[JOIST_MIG_BUF_WORDS];
    struct joist_mig_msg in = *m;
    in.buf = inbuf;
    memcpy(inbuf, m->buf, (size_t)m->len * 8u);
    in.badge = port;
    for (uint32_t i = 0; i < in.ncaps; i++) {
        in.cap[i] = m->cap[i] + 1000u;
        in.mask[i] = 0;
        in.move[i] = 0;
    }
    switch (jt_tamper) {
    case T_EXTRA: /* a word more than the routine takes */
        in.r[0] += (uint64_t)8u << 32;
        in.len = (uint32_t)(((in.r[0] >> 32) + 7u) / 8u > 3u ? ((in.r[0] >> 32) + 7u) / 8u - 3u : 0);
        break;
    case T_LEN: /* a length the words don't match */
        in.len++;
        break;
    case T_ID:
        in.r[0] = (in.r[0] & ~(uint64_t)0xffffffffu) | 0xdeadu;
        break;
    case T_NOCAP:
        in.ncaps = 0;
        break;
    case T_NONUL: /* dir_lookup's name "a": its count, then 'a' and the NUL in r[2] */
        ((unsigned char *)&in.r[2])[1] = 'x';
        break;
    default:
        break;
    }
    int reply_tamper = jt_tamper;
    jt_tamper = T_NONE;
    struct joist_mig_msg out;
    memset(&out, 0, sizeof out);
    out.buf = outbuf;
    /* A spoilt message always through the demuxer: its refusals are the
     * generated code's to check (the lookup's are the transport's). */
    if (jt_calls++ % 2u == 0 || reply_tamper != T_NONE) {
        for (size_t d = 0; d < sizeof demux / sizeof demux[0]; d++) {
            if (demux[d](&in, &out)) {
                break;
            }
        }
    } else {
        joist_mig_routine_t r = NULL;
        for (size_t d = 0; r == NULL && d < sizeof lookup / sizeof lookup[0]; d++) {
            r = lookup[d](&in);
        }
        if (r != NULL) {
            r(&in, &out);
        } else {
            out.r[0] = (uint32_t)JOIST_MIG_BAD_ID; /* as a demuxer answers */
        }
    }
    if (out.len > JOIST_MIG_BUF_WORDS || out.ncaps > JOIST_MIG_CAPS) {
        return 1;
    }
    memcpy(m->r, out.r, sizeof m->r);
    m->len = out.len;
    memcpy(m->buf, outbuf, (size_t)out.len * 8u);
    m->ncaps = reply_tamper == T_NOREPLYCAP ? 0 : out.ncaps; /* a reply without its right */
    for (uint32_t i = 0; i < out.ncaps; i++) {
        m->cap[i] = out.cap[i] + 2000u;
    }
    return 0;
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
    if (failed++ < 20u) {
        fprintf(stderr, "mig-test: %s (result %d): %s\n", routine, r, what);
    }
}

kern_return_t io_seek(io_t io_object, loff_t offset, int whence, loff_t *newp);
kern_return_t io_write(io_t io_object, const_data_t data, mach_msg_type_number_t dataCnt, loff_t offset,
                       vm_size_t *amount);
kern_return_t io_reauthenticate(io_t auth_object, mach_port_t rendezvous2);
kern_return_t dir_lookup(file_t start_dir, const_string_t file_name, int flags, mode_t mode,
                         retry_type *do_retry, string_t retry_name, mach_port_t *result);

static void expect(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "mig-test: %s: %d, expected %d\n", what, got, want);
        failed++;
    }
}

/* Spoilt messages the demuxers must refuse, and an array no message can
 * hold, refused before anything is sent. */
static void directed(void)
{
    loff_t pos;
    vm_size_t amount;
    jt_out_seed = 1;
    expect("io_seek, as sent", io_seek(7, 1, 0, &pos), KERN_SUCCESS);
    jt_tamper = T_EXTRA;
    expect("io_seek, a word too many", io_seek(7, 1, 0, &pos), JOIST_MIG_BAD_ARGUMENTS);
    jt_tamper = T_LEN;
    expect("io_seek, the length wrong", io_seek(7, 1, 0, &pos), JOIST_MIG_BAD_ARGUMENTS);
    jt_tamper = T_ID;
    expect("an unknown routine", io_seek(7, 1, 0, &pos), JOIST_MIG_BAD_ID);
    jt_tamper = T_NOCAP;
    expect("io_reauthenticate, no right", io_reauthenticate(7, 9), JOIST_MIG_BAD_ARGUMENTS);
    retry_type retry;
    string_t name;
    mach_port_t result;
    jt_tamper = T_NOREPLYCAP;
    expect("dir_lookup, its reply without the right", dir_lookup(7, "a", 0, 0, &retry, name, &result),
           JOIST_MIG_TYPE_ERROR);
    jt_tamper = T_NONUL;
    expect("dir_lookup, its name unterminated", dir_lookup(7, "a", 0, 0, &retry, name, &result),
           JOIST_MIG_BAD_ARGUMENTS);
    static char big[JOIST_MIG_WORDS * 8u];
    expect("io_write, more than a message", io_write(7, big, sizeof big, 0, &amount), JOIST_MIG_ARRAY_TOO_LARGE);
}

int main(int argc, char **argv)
{
    unsigned long seeds = argc > 1 ? strtoul(argv[1], NULL, 0) : 200u;
    unsigned calls = 0;
    for (unsigned long s = 1; s <= seeds; s++) {
        (void)jt_io(s, &calls);
        (void)jt_fs(s, &calls);
        (void)jt_fsys(s, &calls);
    }
    directed();
    printf("mig-test: %u calls over %lu seeds, and the spoilt messages; %u failed\n", calls, seeds, failed);
    return failed != 0;
}
