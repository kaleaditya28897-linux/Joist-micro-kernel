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

#endif
