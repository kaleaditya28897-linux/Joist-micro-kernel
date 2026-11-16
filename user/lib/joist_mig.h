/* SPDX-License-Identifier: GPL-3.0-only */
/* joist_mig.h - what code MIG's Joist mode generates needs (Phase 5 step 6;
 * docs/phase5-plan.md, "Step 6: the design").
 *
 * A message (decision 0001): r[0] the routine's id (a request) or its return
 * code (a reply) in its low 32 bits and the body's length in bytes in its
 * high 32, r[1..3] the body's first three words, the rest of the body in the
 * IPC buffer; up to two capabilities. The body is the in-line
 * arguments in declaration order, each 8-byte aligned; a variable array is
 * its count (a word) and then its elements. Generated code makes no system
 * call: a user stub calls joist_mig_call(), which the runtime gives
 * (Joist's user library, later glibc's sysdeps/joist; a fake transport in
 * the host tests), and a server is <prefix>_server(in, out).
 *
 * Header only, freestanding apart from memcpy. */
#ifndef JOIST_MIG_H
#define JOIST_MIG_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define JOIST_MIG_BUF_WORDS 512u                     /* the IPC buffer (0001) */
#define JOIST_MIG_WORDS (3u + JOIST_MIG_BUF_WORDS)    /* a body, at most */
#define JOIST_MIG_CAPS 2u

/* MIG's results (mach/mig_errors.h). */
#define JOIST_MIG_TYPE_ERROR      (-300)
#define JOIST_MIG_REPLY_MISMATCH  (-301)
#define JOIST_MIG_BAD_ID          (-303)
#define JOIST_MIG_BAD_ARGUMENTS   (-304)
#define JOIST_MIG_NO_REPLY        (-305)
#define JOIST_MIG_ARRAY_TOO_LARGE (-307)

/* A right's mask, as the kernel's rights (joist.h's J_R_*). */
#define JOIST_MIG_R_SEND  0x1u
#define JOIST_MIG_R_RECV  0x2u
#define JOIST_MIG_R_GRANT 0x8u

struct joist_mig_msg {
    uint64_t r[4];
    uint32_t ncaps;
    uint32_t cap[JOIST_MIG_CAPS];  /* slots: what is sent, or where it arrived */
    uint8_t mask[JOIST_MIG_CAPS];  /* sending: the rights passed */
    uint8_t move[JOIST_MIG_CAPS];  /* sending: delete the sender's slot after */
    uint32_t len;                  /* words in buf */
    uint64_t *buf;                 /* the IPC buffer: JOIST_MIG_BUF_WORDS */
    uint64_t badge;                /* received: the caller's badge */
};

/* A server's routine (what <demuxer>_routine returns for a message's id;
 * MIG's mig_routine_t, on Joist messages). */
typedef void (*joist_mig_routine_t)(struct joist_mig_msg *in, struct joist_mig_msg *out);

/* The runtime's: a Call on the capability in slot `port`, the reply in
 * *m. 0, or the transport's error (as a kern_return_t). */
int joist_mig_call(uint32_t port, struct joist_mig_msg *m);
/* Memory for data MIG would have passed out of line (vm_allocate), and its
 * release (vm_deallocate). */
void *joist_mig_allocate(size_t bytes);
void joist_mig_deallocate(void *p, size_t bytes);

/* A body being written or read: bytes [0, end), the cursor at. */
struct joist_mig_body {
    uint64_t w[JOIST_MIG_WORDS];
    size_t at, end;
};

static inline size_t joist_mig_align(size_t a)
{
    return (a + 7u) & ~(size_t)7u;
}

/* Append n bytes: -1 if the body is full. */
static inline int joist_mig_put(struct joist_mig_body *b, const void *p, size_t n)
{
    size_t a = joist_mig_align(b->at);
    if (n > sizeof b->w || a > sizeof b->w - n) {
        return -1;
    }
    if (n != 0) {
        memcpy((char *)b->w + a, p, n);
    }
    b->at = a + n;
    b->end = b->at;
    return 0;
}

/* Take n bytes: -1 if the body has fewer left. */
static inline int joist_mig_get(struct joist_mig_body *b, void *p, size_t n)
{
    size_t a = joist_mig_align(b->at);
    if (a > b->end || n > b->end - a) {
        return -1;
    }
    if (n != 0) {
        memcpy(p, (const char *)b->w + a, n);
    }
    b->at = a + n;
    return 0;
}

/* The next n bytes in place (a server's in-line array): NULL if fewer. */
static inline void *joist_mig_ref(struct joist_mig_body *b, size_t n)
{
    size_t a = joist_mig_align(b->at);
    if (b->end < a || b->end - a < n) {
        return NULL;
    }
    b->at = a + n;
    return (char *)b->w + a;
}

/* r[0]'s id or return code. */
static inline uint32_t joist_mig_code(const struct joist_mig_msg *m)
{
    return (uint32_t)m->r[0];
}

/* The body into the message, its length beside the code in r[0]: three
 * words in registers, the rest in the buffer. */
static inline void joist_mig_send(struct joist_mig_msg *m, const struct joist_mig_body *b)
{
    size_t words = joist_mig_align(b->end) / 8u;
    m->r[0] = (uint32_t)m->r[0] | (uint64_t)b->end << 32;
    for (size_t i = 0; i < 3u; i++) {
        m->r[1u + i] = i < words ? b->w[i] : 0;
    }
    m->len = words > 3u ? (uint32_t)(words - 3u) : 0;
    if (m->len != 0) {
        memcpy(m->buf, &b->w[3], (size_t)m->len * 8u);
    }
}

/* A received message's body, to read: -1 if its length is impossible (past
 * what arrived, or words arrived past it). */
static inline int joist_mig_recv(const struct joist_mig_msg *m, struct joist_mig_body *b)
{
    size_t bytes = (size_t)(m->r[0] >> 32);
    size_t words = joist_mig_align(bytes) / 8u;
    if (m->len > JOIST_MIG_BUF_WORDS || m->ncaps > JOIST_MIG_CAPS || words > JOIST_MIG_WORDS ||
        m->len != (words > 3u ? words - 3u : 0)) {
        return -1;
    }
    for (size_t i = 0; i < 3u; i++) {
        b->w[i] = m->r[1u + i];
    }
    if (m->len != 0) {
        memcpy(&b->w[3], m->buf, (size_t)m->len * 8u);
    }
    b->at = 0;
    b->end = bytes;
    return 0;
}

/* A right to send, by its MIG disposition: copy or make a send right, SEND
 * and GRANT; a send-once right, SEND alone; a receive right, every right;
 * a move deletes the sender's. */
static inline int joist_mig_right(struct joist_mig_msg *m, uint32_t name, int disposition)
{
    if (m->ncaps == JOIST_MIG_CAPS) {
        return -1;
    }
    /* MACH_MSG_TYPE_*: MOVE_RECEIVE 16, MOVE_SEND 17, MOVE_SEND_ONCE 18,
     * COPY_SEND 19, MAKE_SEND 20, MAKE_SEND_ONCE 21. */
    int once = disposition == 18 || disposition == 21;
    m->cap[m->ncaps] = name;
    m->mask[m->ncaps] = (uint8_t)(disposition == 16 ? JOIST_MIG_R_SEND | JOIST_MIG_R_RECV | JOIST_MIG_R_GRANT
                                  : once        ? JOIST_MIG_R_SEND
                                                : JOIST_MIG_R_SEND | JOIST_MIG_R_GRANT);
    m->move[m->ncaps] = (uint8_t)(disposition == 16 || disposition == 17 || disposition == 18);
    m->ncaps++;
    return 0;
}

/* ---- bulk data through the caller's windows (decision 0032; Phase 5 step 9) ----
 *
 * A variable array that doesn't fit in line goes through a window: its count
 * word has JOIST_MIG_OOL set and the next word is its byte offset in the
 * window. An in array is in the client's read window, which the server copies
 * from (joist_mig_reply_read); an out array goes into the slice of the
 * client's write window the request offered for it (two words at the
 * array's place in the request: the slice's bytes, 0 for none, and its
 * offset), which the server copies into (joist_mig_reply_write). Each copy
 * call moves at most JOIST_MIG_COPY_MAX bytes; a call carries at most
 * JOIST_MIG_WINDOW_MAX through each window. */
#define JOIST_MIG_COPY_MAX   65536u            /* the kernel's JOIST_COPY_MAX */
#define JOIST_MIG_WINDOW_MAX (1024u * 1024u)   /* a call's most through a window */
#define JOIST_MIG_OOL        ((uint64_t)1 << 63)
#define JOIST_MIG_WIN_READ   0u
#define JOIST_MIG_WIN_WRITE  1u
#define JOIST_MIG_WIN_ARGS   8u                /* windowed arrays a call, each way */
#define JOIST_MIG_PAGE       4096u

/* The runtime's. The calling thread's read or write window over bytes at va
 * (0 bytes: none): 0, or not 0 if refused. */
int joist_mig_window(unsigned kind, const void *va, size_t bytes);
/* Memory a write window may lie over (present, writable, in one of the
 * task's regions), and its release: NULL if the runtime has none, and the
 * stubs then offer no write window. */
void *joist_mig_window_allocate(size_t bytes);
void joist_mig_window_free(void *p, size_t bytes);
/* A server's copies, through the reply it holds for the call it serves:
 * bytes at off in the caller's read window to dst, or from src to off in its
 * write window, at most JOIST_MIG_COPY_MAX; *done the bytes copied. 0, or
 * not 0 if refused. */
int joist_mig_reply_read(uint64_t off, void *dst, size_t bytes, size_t *done);
int joist_mig_reply_write(uint64_t off, const void *src, size_t bytes, size_t *done);

/* Bytes a body has left. */
static inline size_t joist_mig_room(const struct joist_mig_body *b)
{
    size_t a = joist_mig_align(b->at);
    return a > sizeof b->w ? 0 : sizeof b->w - a;
}

/* A client stub's windows for one call. */
struct joist_mig_win {
    unsigned nin, nout;
    const void *in_p[JOIST_MIG_WIN_ARGS];
    size_t in_n[JOIST_MIG_WIN_ARGS];
    size_t in_total;
    size_t out_n[JOIST_MIG_WIN_ARGS];   /* the slices offered (0: none) */
    size_t out_off[JOIST_MIG_WIN_ARGS];
    size_t out_total;
    void *stage;                        /* two in arrays or more, together */
    unsigned char *out;                 /* the write window's memory */
    unsigned set;                       /* the windows set: bit 1 << kind */
};

static inline void joist_mig_win_init(struct joist_mig_win *w)
{
    *w = (struct joist_mig_win){0};
}

/* The next out array's slice of the write window, bytes long (0: none
 * offered). The generated code calls it at most JOIST_MIG_WIN_ARGS times. */
static inline void joist_mig_win_out(struct joist_mig_win *w, size_t bytes)
{
    size_t off = joist_mig_align(w->out_total);
    w->out_off[w->nout] = bytes != 0 ? off : 0;
    w->out_n[w->nout] = bytes;
    w->out_total = bytes != 0 ? off + bytes : w->out_total;
    w->nout++;
}

/* The write window's memory for the slices: if the runtime has none, no
 * slice is offered. */
static inline void joist_mig_win_alloc(struct joist_mig_win *w)
{
    if (w->out_total == 0) {
        return;
    }
    w->out = (unsigned char *)joist_mig_window_allocate(w->out_total);
    if (w->out == NULL) {
        for (unsigned i = 0; i < w->nout; i++) {
            w->out_n[i] = w->out_off[i] = 0;
        }
        w->out_total = 0;
    }
}

/* An in array of bytes at p through the read window: its offset there in
 * *off. -1 past what a call carries. */
static inline int joist_mig_win_in(struct joist_mig_win *w, const void *p, size_t bytes, uint64_t *off)
{
    size_t o = joist_mig_align(w->in_total);
    if (w->nin == JOIST_MIG_WIN_ARGS || bytes == 0 || bytes > JOIST_MIG_WINDOW_MAX ||
        o > JOIST_MIG_WINDOW_MAX - bytes) {
        return -1;
    }
    w->in_p[w->nin] = p;
    w->in_n[w->nin] = bytes;
    w->nin++;
    w->in_total = o + bytes;
    *off = o;
    return 0;
}

/* Before the call: the read window over the in data (the caller's own array,
 * a byte of each page read first, since a page not present stops the
 * kernel's copy; two arrays or more staged together), the write window over
 * its memory. 0, or a MIG error. */
static inline int joist_mig_win_open(struct joist_mig_win *w)
{
    if (w->nin == 1) {
        const volatile unsigned char *p = (const volatile unsigned char *)w->in_p[0];
        size_t n = w->in_n[0];
        size_t lead = (size_t)(JOIST_MIG_PAGE - (uintptr_t)p % JOIST_MIG_PAGE);
        (void)p[0];
        for (size_t i = lead; i < n; i += JOIST_MIG_PAGE) {
            (void)p[i];
        }
        if (joist_mig_window(JOIST_MIG_WIN_READ, w->in_p[0], n) != 0) {
            return JOIST_MIG_ARRAY_TOO_LARGE;
        }
        w->set |= 1u << JOIST_MIG_WIN_READ;
    } else if (w->nin > 1) {
        unsigned char *s = (unsigned char *)joist_mig_allocate(w->in_total);
        if (s == NULL) {
            return JOIST_MIG_ARRAY_TOO_LARGE;
        }
        w->stage = s;
        size_t o = 0;
        for (unsigned i = 0; i < w->nin; i++) {
            o = joist_mig_align(o);
            memcpy(s + o, w->in_p[i], w->in_n[i]);
            o += w->in_n[i];
        }
        if (joist_mig_window(JOIST_MIG_WIN_READ, s, w->in_total) != 0) {
            return JOIST_MIG_ARRAY_TOO_LARGE;
        }
        w->set |= 1u << JOIST_MIG_WIN_READ;
    }
    if (w->out != NULL) {
        if (joist_mig_window(JOIST_MIG_WIN_WRITE, w->out, w->out_total) != 0) {
            return JOIST_MIG_ARRAY_TOO_LARGE;
        }
        w->set |= 1u << JOIST_MIG_WIN_WRITE;
    }
    return 0;
}

/* After the call: both windows cleared. */
static inline void joist_mig_win_close(struct joist_mig_win *w)
{
    for (unsigned k = 0; k < 2u; k++) {
        if (w->set & 1u << k) {
            (void)joist_mig_window(k, NULL, 0);
        }
    }
    w->set = 0;
}

/* When the stub returns (its cleanup): the windows cleared, the memory freed. */
static inline void joist_mig_win_done(struct joist_mig_win *w)
{
    joist_mig_win_close(w);
    if (w->stage != NULL) {
        joist_mig_deallocate(w->stage, w->in_total);
        w->stage = NULL;
    }
    if (w->out != NULL) {
        joist_mig_window_free(w->out, w->out_total);
        w->out = NULL;
    }
}

/* A reply's windowed array, bytes long at off, for slice i: where it is, or
 * NULL if that isn't inside the slice offered. */
static inline const void *joist_mig_win_result(const struct joist_mig_win *w, unsigned i, uint64_t off,
                                               size_t bytes)
{
    if (w->out == NULL || i >= w->nout || w->out_n[i] == 0 || off != w->out_off[i] || bytes > w->out_n[i]) {
        return NULL;
    }
    return w->out + off;
}

/* A server stub's in array that came through the window: memory of its own,
 * freed when the stub returns (its cleanup). */
struct joist_mig_ool {
    void *p;
    size_t n;
};

static inline void joist_mig_ool_free(struct joist_mig_ool *o)
{
    if (o->p != NULL) {
        joist_mig_deallocate(o->p, o->n);
        o->p = NULL;
    }
}

/* o->n bytes at off in the caller's read window into memory of the stub's
 * own, a copy call at a time: 0, or -1 (refused, or short). */
static inline int joist_mig_ool_read(struct joist_mig_ool *o, uint64_t off)
{
    if (o->n == 0 || o->n > JOIST_MIG_WINDOW_MAX || off > JOIST_MIG_WINDOW_MAX) {
        return -1;
    }
    o->p = joist_mig_allocate(o->n);
    if (o->p == NULL) {
        return -1;
    }
    for (size_t at = 0; at < o->n;) {
        size_t k = o->n - at < JOIST_MIG_COPY_MAX ? o->n - at : JOIST_MIG_COPY_MAX;
        size_t done = 0;
        if (joist_mig_reply_read(off + at, (unsigned char *)o->p + at, k, &done) != 0 || done != k) {
            return -1;
        }
        at += k;
    }
    return 0;
}

/* bytes from src to off in the caller's write window, a copy call at a time:
 * 0, or -1 (refused, or short). */
static inline int joist_mig_write_all(uint64_t off, const void *src, size_t bytes)
{
    if (off > JOIST_MIG_WINDOW_MAX) {
        return -1;
    }
    for (size_t at = 0; at < bytes;) {
        size_t k = bytes - at < JOIST_MIG_COPY_MAX ? bytes - at : JOIST_MIG_COPY_MAX;
        size_t done = 0;
        if (joist_mig_reply_write(off + at, (const unsigned char *)src + at, k, &done) != 0 || done != k) {
            return -1;
        }
        at += k;
    }
    return 0;
}

#endif
