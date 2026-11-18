/* SPDX-License-Identifier: GPL-3.0-only */
/* transport.c - the round-trip test's fake transport and main (Phase 5 step
 * 6): joist_mig_call() does what the kernel's Call does to a message
 * (registers, the IPC buffer copied whole, capabilities moved to other
 * slots: +1000 on the way in, +2000 on the way back) and hands it to the
 * interfaces' demuxers; the runtime's memory is malloc. Then every carried
 * routine of io, fs and fsys, once per seed, and a few messages spoilt on
 * the way (jt_tamper) that the demuxers must refuse.
 *
 * Decision 0032's windows as the kernel keeps them (Phase 5 step 9): the
 * calling thread's read and write windows (a write window only over memory
 * joist_mig_window_allocate gave: the kernel's "in one of its regions"),
 * and the server's copies through the call's reply, refused unless the
 * caller waits on it (E_PEER), with no window (E_STATE), past the window
 * or past JOIST_MIG_COPY_MAX (E_RANGE); and, spoilt on purpose, a copy that
 * stops short (a page not present) or a caller gone. */
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
enum { T_NONE, T_EXTRA, T_LEN, T_ID, T_NOCAP, T_NOREPLYCAP, T_NONUL, T_SHORT, T_GONE, T_REPLYOFF };
static int jt_tamper;

/* ---- the kernel's windows (0032) ---- */
enum { E_OK = 0, E_RANGE = 4, E_STATE = 6, E_PEER = 11 }; /* the kernel's codes, as the runtime returns them */
static struct {
    const unsigned char *va;
    size_t n;
} win[2];
static int waiting;   /* a call in progress: its caller waits on the reply */
static int copy_short; /* the next copy stops halfway (a page not present) */
static struct {
    void *p;
    size_t n;
} wmem[16]; /* joist_mig_window_allocate's, live: the "regions" */
static int no_window_memory; /* the runtime has none to give */
static unsigned long copies[2], pieces_full, copied[2], wins_set;

int joist_mig_window(unsigned kind, const void *va, size_t bytes)
{
    if (kind > JOIST_MIG_WIN_WRITE) {
        return E_RANGE;
    }
    if (bytes == 0) {
        win[kind].va = NULL;
        win[kind].n = 0;
        return E_OK;
    }
    if (kind == JOIST_MIG_WIN_WRITE) {
        int in_region = 0;
        for (size_t i = 0; i < sizeof wmem / sizeof wmem[0]; i++) {
            const unsigned char *b = wmem[i].p;
            in_region |= b != NULL && (const unsigned char *)va >= b && bytes <= wmem[i].n &&
                         (size_t)((const unsigned char *)va - b) <= wmem[i].n - bytes;
        }
        if (!in_region) {
            return E_RANGE;
        }
    }
    win[kind].va = va;
    win[kind].n = bytes;
    wins_set++;
    return E_OK;
}

void *joist_mig_window_allocate(size_t bytes)
{
    if (no_window_memory) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof wmem / sizeof wmem[0]; i++) {
        if (wmem[i].p == NULL) {
            wmem[i].p = malloc(bytes != 0 ? bytes : 1u);
            wmem[i].n = bytes;
            return wmem[i].p;
        }
    }
    return NULL;
}

void joist_mig_window_free(void *p, size_t bytes)
{
    for (size_t i = 0; i < sizeof wmem / sizeof wmem[0]; i++) {
        if (wmem[i].p == p && p != NULL) {
            if (wmem[i].n != bytes) {
                jt_fail("joist_mig_window_free", (int)bytes, "freed with another size than it was given");
            }
            free(p);
            wmem[i].p = NULL;
            return;
        }
    }
    jt_fail("joist_mig_window_free", 0, "not memory joist_mig_window_allocate gave");
}

/* A copy through the call's reply: the kernel's checks, then the bytes. */
static int copy(unsigned kind, uint64_t off, unsigned char *dst, const unsigned char *src, size_t bytes, size_t *done)
{
    *done = 0;
    if (!waiting) {
        return E_PEER;
    }
    if (win[kind].va == NULL) {
        return E_STATE;
    }
    if (bytes > JOIST_MIG_COPY_MAX || off > win[kind].n || bytes > win[kind].n - off) {
        return E_RANGE;
    }
    size_t k = copy_short ? bytes / 2u : bytes;
    copy_short = 0;
    if (kind == JOIST_MIG_WIN_READ) {
        memcpy(dst, win[kind].va + off, k);
    } else {
        memcpy((unsigned char *)(uintptr_t)win[kind].va + off, src, k);
    }
    *done = k;
    copies[kind]++;
    copied[kind] += k;
    pieces_full += bytes == JOIST_MIG_COPY_MAX;
    return E_OK;
}

int joist_mig_reply_read(uint64_t off, void *dst, size_t bytes, size_t *done)
{
    return copy(JOIST_MIG_WIN_READ, off, dst, NULL, bytes, done);
}

int joist_mig_reply_write(uint64_t off, const void *src, size_t bytes, size_t *done)
{
    return copy(JOIST_MIG_WIN_WRITE, off, NULL, src, bytes, done);
}

void jt_check_windows(const char *routine)
{
    if (win[0].va != NULL || win[1].va != NULL) {
        jt_fail(routine, 0, "a window left set after the call");
        win[0].va = win[1].va = NULL;
    }
}

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
    joist_mig_nulls_out(m); /* the client's runtime: null ports marked, not sent */
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
    case T_SHORT: /* the server's first copy stops short */
        copy_short = 1;
        break;
    default:
        break;
    }
    (void)joist_mig_nulls_in(&in); /* the server's runtime: the nulls back */
    waiting = jt_tamper != T_GONE; /* T_GONE: the caller has stopped waiting */
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
    waiting = 0;
    copy_short = 0;
    if (reply_tamper == T_GONE) {
        return 11; /* no answer comes: as the kernel's E_PEER to a caller */
    }
    if (reply_tamper == T_REPLYOFF && (out.r[1] & JOIST_MIG_OOL) != 0) {
        out.r[2] += 8u; /* a windowed array's offset, moved */
    }
    if (out.len > JOIST_MIG_BUF_WORDS || out.ncaps > JOIST_MIG_CAPS) {
        return 1;
    }
    joist_mig_nulls_out(&out); /* the server's runtime, answering */
    memcpy(m->r, out.r, sizeof m->r);
    m->len = out.len;
    memcpy(m->buf, outbuf, (size_t)out.len * 8u);
    m->ncaps = reply_tamper == T_NOREPLYCAP ? 0 : out.ncaps; /* a reply without its right */
    for (uint32_t i = 0; i < out.ncaps; i++) {
        m->cap[i] = out.cap[i] + 2000u;
    }
    (void)joist_mig_nulls_in(m); /* the client's runtime: the reply's nulls back */
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
kern_return_t io_read(io_t io_object, data_t *data, mach_msg_type_number_t *dataCnt, loff_t offset,
                      vm_size_t amount);
kern_return_t dir_lookup(file_t start_dir, const_string_t file_name, int flags, mode_t mode,
                         retry_type *do_retry, string_t retry_name, mach_port_t *result);

static void expect(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "mig-test: %s: %d, expected %d\n", what, got, want);
        failed++;
    }
}

static void windows(void);

/* Spoilt messages the demuxers must refuse, and an array no call can carry,
 * refused before anything is sent; then the windows' edges. */
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
    /* The body's own bound (the stubs check the room first, so it is
     * reached only from here). */
    struct joist_mig_body b;
    static char z[JOIST_MIG_WORDS * 8u];
    b.at = b.end = 0;
    expect("joist_mig_put: a body's bytes and no more",
           joist_mig_put(&b, z, 4096u) == 0 && joist_mig_put(&b, z, 4096u) == -1 &&
               joist_mig_put(&b, z, sizeof z - 4096u) == 0 && joist_mig_put(&b, z, 1u) == -1,
           1);
    static char big[JOIST_MIG_WINDOW_MAX + 1u];
    expect("io_write, past what a call carries", io_write(7, big, sizeof big, 0, &amount), JOIST_MIG_ARRAY_TOO_LARGE);
    windows();
}

/* A reply's data, windowed or in line, from io_read with seed s: its count
 * (KERN_SUCCESS), or -1 if it failed with r. */
static long read_one(uint64_t s, vm_size_t amount, int *r)
{
    char small[16];
    data_t data = small;
    mach_msg_type_number_t n = sizeof small;
    jt_out_seed = s;
    *r = io_read(7, &data, &n, 0, amount);
    if (*r != KERN_SUCCESS) {
        return -1;
    }
    if (data != small) {
        joist_mig_deallocate(data, n);
    }
    return (long)n;
}

/* Decision 0032's windows, at their edges (the round trip covers the rest). */
static void windows(void)
{
    vm_size_t amount;
    static char data[100000];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (char)(i * 7u);
    }
    jt_out_seed = 1;
    expect("io_write of 100000 bytes: through the read window, in two copies", io_write(7, data, sizeof data, 0, &amount),
           KERN_SUCCESS);
    jt_tamper = T_SHORT;
    expect("io_write, its server's copy short (a page not present)", io_write(7, data, sizeof data, 0, &amount),
           JOIST_MIG_BAD_ARGUMENTS);
    jt_tamper = T_GONE;
    expect("io_write, its caller gone before the server's copy", io_write(7, data, sizeof data, 0, &amount), 11);
    jt_check_windows("io_write");

    /* io_read: a seed whose server returns more than fits in line. */
    uint64_t s = 1;
    int r;
    long n = 0;
    for (; s < 1000u; s++) {
        if (s % 13u != 0 && (n = read_one(s, 140000u, &r)) > 4200) {
            break;
        }
    }
    expect("io_read: a reply past the IPC buffer found", n > 4200, 1);
    unsigned long writes = copies[JOIST_MIG_WIN_WRITE];
    expect("io_read, again", read_one(s, 140000u, &r) == n, 1);
    expect("io_read: its data through the write window", copies[JOIST_MIG_WIN_WRITE] > writes, 1);
    jt_tamper = T_SHORT;
    (void)read_one(s, 140000u, &r);
    expect("io_read, its server's copy short", r, JOIST_MIG_BAD_ARGUMENTS);
    jt_tamper = T_REPLYOFF;
    (void)read_one(s, 140000u, &r);
    expect("io_read, its reply's offset outside the slice", r, JOIST_MIG_TYPE_ERROR);
    jt_tamper = T_GONE;
    (void)read_one(s, 140000u, &r);
    expect("io_read, its caller gone before the server's copy", r, 11);
    no_window_memory = 1;
    (void)read_one(s, 140000u, &r);
    expect("io_read, no memory for a write window: as before 0032", r, JOIST_MIG_ARRAY_TOO_LARGE);
    no_window_memory = 0;
    /* A bound past JOIST_MIG_WINDOW_MAX: io_read may return less, so the
     * stub lowers it, and the server sees the lowered one. */
    uint64_t h = JT_H0;
    uint32_t port = 7;
    loff_t off = 0;
    vm_size_t lowered = JOIST_MIG_WINDOW_MAX;
    h = jt_mix(h, &port, 4);
    h = jt_mix(h, &off, sizeof off);
    h = jt_mix(h, &lowered, sizeof lowered);
    (void)read_one(1, 64u << 20, &r);
    expect("io_read of 64 MiB", r, KERN_SUCCESS);
    expect("io_read of 64 MiB: the server asked for JOIST_MIG_WINDOW_MAX", jt_seen == h, 1);
    jt_check_windows("io_read");
    for (size_t i = 0; i < sizeof wmem / sizeof wmem[0]; i++) {
        expect("a write window's memory freed", wmem[i].p == NULL, 1);
    }
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
    /* The round trip must have used both windows, and copies of the most a
     * call moves (its loops). */
    expect("the round trip copied from read windows", copies[JOIST_MIG_WIN_READ] != 0, 1);
    expect("the round trip copied into write windows", copies[JOIST_MIG_WIN_WRITE] != 0, 1);
    expect("the round trip copied JOIST_MIG_COPY_MAX at once", pieces_full != 0, 1);
    printf("mig-test: windows: %lu set, %lu reads (%lu bytes), %lu writes (%lu bytes), %lu of %u bytes\n", wins_set,
           copies[0], copied[0], copies[1], copied[1], pieces_full, JOIST_MIG_COPY_MAX);
    directed();
    printf("mig-test: %u calls over %lu seeds, and the spoilt messages; %u failed\n", calls, seeds, failed);
    return failed != 0;
}
