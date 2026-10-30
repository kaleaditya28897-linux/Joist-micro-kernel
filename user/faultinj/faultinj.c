/* SPDX-License-Identifier: GPL-3.0-only */
/* faultinj.c - the fault-injection test (Phase 3's exit criterion;
 * docs/phase3-plan.md, *Testing*): clients and servers killed at points
 * chosen from a seed, and checks that every survivor sees exactly what
 * docs/abi.md says it sees, and that nothing hangs.
 *
 * Module 0 is fiworker, loaded as every peer task of a round. A round
 * builds a small world of tasks, drives their threads command by command
 * through the pages it shares with them (lib/fi.h) to a kill point, kills
 * one of them in one of the ways the ABI offers (thread_kill, deleting the
 * last thread capability, task_destroy, deleting the last task capability;
 * and, where that is the point, deleting a reply object or a capability),
 * and checks what the others get. The scenarios, one per round in turn:
 *
 * - call: a client killed in the middle of a Call, before the server
 *   received it (its message must be gone) or while the server holds its
 *   reply capability (the server's Reply is E_PEER); the server goes on to
 *   the next caller; no-senders for the client's badge fires when its task
 *   goes, not before, once.
 * - server: a server killed holding a reply capability (or around its
 *   Recv; or passive, running on its caller's time): the caller gets
 *   E_PEER and goes on, on its own time; callers queued behind get E_GONE
 *   when the endpoint dies, which is when its task goes, not its thread;
 *   the dead-name notification comes then, once; every call after is
 *   E_GONE.
 * - saved: a server killed holding a saved reply (reply_save): the thread
 *   alone dying changes nothing (the reply object outlives it, and another
 *   thread answers); the task dying, or the object's deletion, is E_PEER.
 *   Killed before saving, it is E_PEER.
 * - transfer: a capability in a message when its sender or its receiver
 *   dies, or when it is deleted after the send: the message goes with a
 *   dead sender, or E_GONE with a dead receiver; a delivered copy keeps the
 *   badge alive; a deleted one is skipped at delivery; no-senders fires
 *   when the last copy goes, once.
 * - signal: a thread killed while signalling: exactly the bits it got to
 *   signal arrive, each once; a waiter killed while waiting: nothing sent
 *   after is lost.
 *
 * Every wait of the root's is bounded: a survivor that doesn't get what it
 * is owed in time hangs, which is a failure. At the end of a round every
 * thread still alive must have finished its last command, unless it was
 * left in a call the ABI lets wait for ever (a Recv on a live endpoint),
 * which an abort must then end; every task is destroyed and the root
 * checks none is left. The next round reuses the pages and slots.
 *
 * The seed is FAULTINJ_SEED, and the rounds are FAULTINJ_FIRST onwards,
 * FAULTINJ_ROUNDS of them (UEXTRA="-DFAULTINJ_SEED=n ..."). A round's
 * choices depend on the seed and its number alone, so a failure, which
 * prints both, replays as one round. Which thread gets where first on
 * several CPUs is not the seed's to choose: where the order is open, the
 * checks accept exactly the outcomes the ABI allows for each order, and the
 * counts say which happened.
 *
 * Peers run below the root (servers at 90, clients 100, a waiter 110): on
 * one CPU they run while it waits. */
#define TEST_NAME "faultinj"
#include "../lib/fi.h"
#include "../lib/testroot.h"

#ifndef FAULTINJ_SEED
#define FAULTINJ_SEED 1u
#endif
#ifndef FAULTINJ_FIRST
#define FAULTINJ_FIRST 0u
#endif
#ifndef FAULTINJ_ROUNDS
#define FAULTINJ_ROUNDS 100u
#endif

#define PEER_VA    0x38000000ull            /* the peers' pages, as the root sees them */
#define STACK2     (CHILD_LO - 6u * J_PAGE) /* a second thread's stack, below the first */
#define MAX_PEERS  3u
#define WARM_PAGES 48u /* more than any round takes (it checks) */
#define RSLOTS     48u /* the root's slots for a round's objects */
#define POLL_NS    J_MS
#define SETTLE_NS  (2 * J_MS)    /* for a thread that has begun a call to be in it */
#define STILL_NS   (20 * J_MS)   /* how long what mustn't happen is watched for */
#define BOUND_NS   (3000 * J_MS) /* longer than anything owed takes: past it, a hang */
#define NS_BIT     0x10u         /* the bit no-senders signals */
#define DN_BIT     0x20u         /* the bit a dead name signals */
#define SLOT_NONE  0xffffu       /* a capability skipped at delivery, in the caps word */

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

/* Kill points, ways to kill, and what the survivors saw: counted, and
 * printed at the end. */
enum {
    KP_CALL_QUEUED,
    KP_CALL_HELD,
    KP_SERVER_EARLY,
    KP_SERVER_HOLDING,
    KP_SERVER_PASSIVE,
    KP_SAVED_HOLD,
    KP_SAVED,
    KP_XFER_QUEUED,
    KP_XFER_DELIVERED,
    KP_XFER_RECEIVER,
    KP_XFER_LOST,
    KP_SIGNALLER,
    KP_WAITER,
    NKP
};
static const char *const kp_name[NKP] = {
    "a client in a Call not yet received",
    "a client in a Call its server holds",
    "a server around its Recv",
    "a server holding a reply capability",
    "a passive server holding a reply capability, on its caller's time",
    "a server holding a reply capability it hasn't saved yet",
    "a server holding a saved reply",
    "a sender with a capability in a queued Call",
    "a sender whose capability was delivered",
    "a receiver with a capability on its way to it",
    "a capability deleted between its send and its delivery",
    "a thread while signalling",
    "a thread waiting for signals",
};
enum { K_THREAD, K_THREADCAP, K_DESTROY, K_TASKCAP, K_REPLYOBJ, K_CAPDEL, NK };
static const char *const how_name[NK] = {
    "thread_kill",          "last thread capability deleted",
    "task_destroy",         "last task capability deleted",
    "reply object deleted", "capability deleted",
};
enum {
    O_PEER,
    O_GONE,
    O_WITHDRAWN,
    O_WENT_ON,
    O_SAVED_OK,
    O_NOSENDERS,
    O_DEADNAME,
    O_SKIPPED,
    O_REFUSED,
    O_SIGNALS_EXACT,
    O_SIGNALS_EDGE,
    O_ABORTED,
    NO
};
static const char *const out_name[NO] = {
    "E_PEER",
    "E_GONE",
    "a killed caller's message withdrawn",
    "a caller going on after E_PEER",
    "a saved reply answered after its thread died",
    "no-senders, each once",
    "dead names, each once",
    "a capability skipped at delivery",
    "a Call failed at its send (the capability already gone)",
    "signals exactly as sent",
    "signals as sent, one in flight at the kill",
    "threads left in a call for ever, aborted",
};
enum { SC_CALL, SC_SERVER, SC_SAVED, SC_TRANSFER, SC_SIGNAL, NSC };
static const char *const sc_name[NSC] = {"call", "server", "saved", "transfer", "signal"};

static unsigned count[NKP][NK];
static unsigned outcome[NO];
static unsigned rounds[NSC];

struct peer {
    uint64_t task, thread[2], sc[2], go[2], gos[2];
    unsigned nthreads;
    int live;      /* loaded this round */
    int task_gone; /* destroyed by the root */
    int gone[2];   /* killed by the root */
    int parked[2]; /* may be left in a call for ever */
    struct fi_page *pg;
};
static struct peer peers[MAX_PEERS];
static uint64_t rslot[RSLOTS];
static unsigned nrs;
static uint64_t per_ms; /* time stamp counts to the millisecond */

/* Where the round is, for a failure to say. */
static uint64_t round_no;
static unsigned cur_sc = NSC, cur_kp = NKP, cur_how = NK;

static void put(const char *s, uint64_t v)
{
    j_puts(s);
    j_putu(v);
}

static __attribute__((noreturn)) void fail_code(const char *what, int has_code, uint64_t code)
{
    put("faultinj: FAILED: seed ", FAULTINJ_SEED);
    put(" round ", round_no);
    j_puts(" (");
    j_puts(cur_sc < NSC ? sc_name[cur_sc] : "setup");
    if (cur_kp < NKP) {
        j_puts("; ");
        j_puts(kp_name[cur_kp]);
    }
    if (cur_how < NK) {
        j_puts("; ");
        j_puts(how_name[cur_how]);
    }
    j_puts("): ");
    j_puts(what);
    if (has_code) {
        put(": result ", code);
    }
    put("\nfaultinj: replay: UEXTRA=\"-DFAULTINJ_SEED=", FAULTINJ_SEED);
    put(" -DFAULTINJ_FIRST=", round_no);
    j_puts(" -DFAULTINJ_ROUNDS=1\"\n");
    j_exit(1);
}

static __attribute__((noreturn)) void fail(const char *what)
{
    fail_code(what, 0, 0);
}

static void expect(int ok, const char *what)
{
    if (!ok) {
        fail(what);
    }
}

/* A call of the root's own that must succeed. */
static void need(uint64_t e, const char *what)
{
    if (e != J_OK) {
        fail_code(what, 1, e);
    }
}

/* The seed's choices: splitmix64, started afresh from the seed and the
 * round's number at every round. */
static uint64_t rng;
static uint64_t rnd(void)
{
    uint64_t z = (rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static unsigned below(unsigned n)
{
    return (unsigned)(rnd() % n);
}

/* A moment of the seed's choosing before a kill: none, a short sleep (the
 * peers run), a short spin (they run only on other CPUs), or long enough
 * for everything to settle. */
static void jitter(void)
{
    switch (below(4)) {
    case 0:
        break;
    case 1:
        sleep_ns(rnd() % (2 * J_MS));
        break;
    case 2: {
        uint64_t until = work_tsc() + rnd() % (per_ms / 4u + 1u);
        while (work_tsc() < until) {
        }
        break;
    }
    default:
        sleep_ns(SETTLE_NS + rnd() % (5 * J_MS));
        break;
    }
}

static int task_level(unsigned how)
{
    return how == K_DESTROY || how == K_TASKCAP;
}

static uint64_t mask_of(uint64_t n)
{
    return n >= 64u ? ~0ull : (1ull << n) - 1u;
}

/* A slot of the root's for this round; all are emptied at its end. */
static uint64_t rs(void)
{
    if (nrs == RSLOTS) {
        fail("a round needs more slots than the root keeps for it");
    }
    return rslot[nrs++];
}
static uint64_t obj(uint64_t type)
{
    uint64_t s = rs();
    need(j_obj_create(type, s), "obj_create");
    return s;
}
static uint64_t mint(uint64_t src, uint64_t mask, uint64_t badge)
{
    uint64_t s = rs();
    need(j_cap_mint(src, s, mask, badge), "cap_mint");
    return s;
}
static uint64_t derive(uint64_t src, uint64_t mask)
{
    uint64_t s = rs();
    need(j_cap_derive(src, s, mask), "cap_derive");
    return s;
}
static void give(const struct peer *p, uint64_t src, uint64_t dst, uint64_t mask)
{
    need(j_cap_give(p->task, src, dst, mask), "cap_give");
}
/* Give, and delete the root's copy: the peer's is then the only one. */
static void hand(const struct peer *p, uint64_t src, uint64_t dst, uint64_t mask)
{
    give(p, src, dst, mask);
    need(j_cap_delete(src), "cap_delete");
}

/* Peer i: a task of fiworker with nthreads threads at those priorities,
 * each running on a context of its own and waiting for commands. */
static struct peer *peer_new(unsigned i, unsigned nthreads, uint64_t prio0, uint64_t prio1)
{
    struct peer *p = &peers[i];
    p->live = 1;
    p->task_gone = 0;
    p->nthreads = nthreads;
    for (unsigned j = 0; j < 2; j++) {
        p->gone[j] = 0;
        p->parked[j] = 0;
    }
    uint64_t entry = ld_load(0, p->task);
    if (nthreads == 2) {
        ld_stack(p->task, STACK2);
    }
    uint64_t f = page_alloc();
    uint64_t va = PEER_VA + i * J_PAGE;
    need(j_map(f, J_ROOT_SELF, va / J_PAGE, J_MAP_W), "map (peer page, here)");
    volatile uint64_t *w = (volatile uint64_t *)(uintptr_t)va;
    for (unsigned k = 0; k < J_PAGE / 8u; k++) {
        w[k] = 0; /* a page given back keeps what the last peer left */
    }
    p->pg = (struct fi_page *)(uintptr_t)va;
    need(j_map(f, p->task, FI_VA / J_PAGE, J_MAP_W), "map (peer page, there)");
    for (unsigned j = 0; j < nthreads; j++) {
        need(j_thread_spawn(p->task, p->thread[j], entry, j == 0 ? CHILD_LO : STACK2),
             "thread_spawn");
        if (j == 1) {
            struct j_thread_regs regs;
            need(j_thread_read_regs(p->thread[j], &regs), "thread_read_regs");
            regs.r[J_RDI] = 1; /* its record in the page */
            need(j_thread_write_regs(p->thread[j], &regs), "thread_write_regs");
        }
        need(j_obj_create(J_NOTIFICATION, p->go[j]), "obj_create (go)");
        need(j_cap_give(p->task, p->go[j], FI_GO0 + j, J_R_RECV), "cap_give (go)");
        need(j_cap_mint(p->go[j], p->gos[j], J_R_SEND, 1), "cap_mint (go)");
        need(j_sc_create(p->sc[j], j == 0 ? prio0 : prio1, 10 * J_MS, 10 * J_MS), "sc_create");
        need(j_sc_bind(p->sc[j], p->thread[j]), "sc_bind");
    }
    return p;
}

static struct fi_thread *th(const struct peer *p, unsigned j)
{
    return &p->pg->t[j];
}

/* Give thread j of p its next command. */
static void post(const struct peer *p, unsigned j, uint64_t cmd, uint64_t a0, uint64_t a1,
                 uint64_t a2, uint64_t a3)
{
    struct fi_thread *t = th(p, j);
    if (t->done != t->posted) {
        fail("a command for a thread still busy with its last (the test's own mistake)");
    }
    t->cmd = cmd;
    t->a[0] = a0;
    t->a[1] = a1;
    t->a[2] = a2;
    t->a[3] = a3;
    t->posted = t->posted + 1u;
    need(j_signal(p->gos[j]), "signal (go)");
}

/* Has the thread finished its last command, within ns? */
static int finished(const struct peer *p, unsigned j, uint64_t ns)
{
    const struct fi_thread *t = th(p, j);
    for (uint64_t waited = 0;; waited += POLL_NS) {
        if (t->done == t->posted) {
            return 1;
        }
        if (waited >= ns) {
            return 0;
        }
        sleep_ns(POLL_NS);
    }
}

/* What its last command returned, which it is owed within the bound: if
 * it doesn't come, the thread hangs (`what`). */
static uint64_t result(const struct peer *p, unsigned j, const char *what)
{
    if (!finished(p, j, BOUND_NS)) {
        fail(what);
    }
    return th(p, j)->res;
}

/* Until the thread has begun its last command, and a moment more: it is
 * then in the call it was about to make. */
static void begun(const struct peer *p, unsigned j)
{
    const struct fi_thread *t = th(p, j);
    for (uint64_t waited = 0; t->started != t->posted; waited += POLL_NS) {
        if (waited >= BOUND_NS) {
            fail("a thread didn't begin its command");
        }
        sleep_ns(POLL_NS);
    }
    sleep_ns(SETTLE_NS);
}

static void kill(struct peer *p, unsigned j, unsigned how)
{
    switch (how) {
    case K_THREAD:
        need(j_thread_kill(p->thread[j]), "thread_kill");
        p->gone[j] = 1;
        break;
    case K_THREADCAP:
        need(j_cap_delete(p->thread[j]), "cap_delete (the thread's last capability)");
        p->gone[j] = 1;
        break;
    case K_DESTROY:
        need(j_task_destroy(p->task), "task_destroy");
        p->task_gone = 1;
        break;
    default:
        need(j_cap_delete(p->task), "cap_delete (the task's last capability)");
        p->task_gone = 1;
        break;
    }
    count[cur_kp][how]++;
    /* A destruction ends by itself (docs/abi.md, *Task destruction*); in
     * some rounds the root helps it along, in others not. */
    if (task_level(how) && below(2)) {
        int more = 1;
        while (more) {
            need(j_continue(0, &more), "continue");
        }
    }
}

static void destroy(struct peer *p)
{
    need(j_task_destroy(p->task), "task_destroy");
    p->task_gone = 1;
}

/* Did notification n (the root's, RECV) bring `bit`, and nothing else,
 * within ns? */
static int fired(uint64_t n, uint64_t bit, uint64_t ns)
{
    uint64_t bits;
    uint64_t e = j_wait(n, ns, &bits);
    if (e == J_E_TIMEOUT) {
        return 0;
    }
    if (e != J_OK) {
        fail_code("a Wait on the root's notification failed", 1, e);
    }
    if (bits != bit) {
        fail_code("a notification brought bits nobody signalled", 1, bits);
    }
    return 1;
}
/* Nothing came to n for a while. */
static int quiet(uint64_t n)
{
    uint64_t bits;
    return j_wait(n, STILL_NS, &bits) == J_E_TIMEOUT;
}

/* The next badge in an endpoint's no-senders queue: the server peer s
 * collects it, or the root if s is NULL. */
static uint64_t ns_next(const struct peer *s, uint64_t ep)
{
    uint64_t b;
    if (s == NULL) {
        need(j_nosenders_next(ep, &b), "nosenders_next");
        return b;
    }
    post(s, 0, FI_NSNEXT, ep, 0, 0, 0);
    uint64_t e = result(s, 0, "nosenders_next hangs");
    if (e != J_OK) {
        fail_code("nosenders_next failed", 1, e);
    }
    return th(s, 0)->val[0];
}

/* No-senders fired for `badge`, once: the summary bit came, the badge is
 * queued, and nothing else. */
static void ns_fired(uint64_t n, const struct peer *s, uint64_t ep, uint64_t badge)
{
    expect(fired(n, NS_BIT, BOUND_NS),
           "no no-senders notification when a badge's last sender went");
    expect(ns_next(s, ep) == badge,
           "no-senders fired, but the queue doesn't hold the badge whose last sender went");
    expect(ns_next(s, ep) == 0, "a badge was queued twice by no-senders");
    expect(quiet(n), "no-senders signalled twice");
    outcome[O_NOSENDERS]++;
}
static void ns_quiet(uint64_t n, const struct peer *s, uint64_t ep, const char *what)
{
    expect(quiet(n), what);
    expect(ns_next(s, ep) == 0, what);
}

static void dn_fired(uint64_t n)
{
    expect(fired(n, DN_BIT, BOUND_NS), "no dead-name notification when the endpoint died");
    expect(quiet(n), "a dead-name notification came twice");
    outcome[O_DEADNAME]++;
}

/* An endpoint whose only RECV goes to the server s, its own capability
 * deleted; the root keeps one to send on, in *er, with a dead-name
 * registration signalling dn (if er is not NULL). */
static uint64_t endpoint_for(const struct peer *s, uint64_t *er, uint64_t *dn)
{
    uint64_t ep = obj(J_ENDPOINT);
    give(s, ep, FI_EP, J_R_RECV);
    if (er != NULL) {
        *er = derive(ep, J_R_SEND);
        *dn = obj(J_NOTIFICATION);
        need(j_request_deadname(*er, mint(*dn, J_R_SEND, DN_BIT)), "request_deadname");
    }
    return ep;
}

/* 1. A client killed in the middle of a Call. */
static void sc_call(void)
{
    struct peer *s = peer_new(0, 1, 90, 0);
    struct peer *c = peer_new(1, 1, 100, 0);
    struct peer *b = peer_new(2, 1, 100, 0);
    uint64_t ep = endpoint_for(s, NULL, NULL);
    uint64_t ns = obj(J_NOTIFICATION);
    hand(c, mint(ep, J_R_SEND | J_R_GRANT, 1), FI_EP, J_R_SEND);
    hand(b, mint(ep, J_R_SEND | J_R_GRANT, 2), FI_EP, J_R_SEND);
    need(j_cap_delete(ep), "cap_delete"); /* the server's is the only RECV */
    hand(s, mint(ns, J_R_SEND | J_R_GRANT, NS_BIT), FI_NS, J_R_SEND);
    post(s, 0, FI_BIND_NS, FI_EP, FI_NS, 0, 0);
    expect(result(s, 0, "bind_nosenders hangs") == J_OK, "bind_nosenders failed");

    unsigned how = below(4);
    cur_how = how;
    if (below(2)) {
        cur_kp = KP_CALL_HELD;
        post(s, 0, FI_RECV, FI_EP, J_FOREVER, 0, 0);
        begun(s, 0);
        post(c, 0, FI_CALL, FI_EP, 11, FI_NOCAP, J_FOREVER);
        expect(result(s, 0, "a server's Recv hangs with a caller there") == J_OK &&
                   th(s, 0)->val[0] == 1 && th(s, 0)->val[1] == 11,
               "the server didn't receive its caller's message and badge");
        jitter();
        kill(c, 0, how);
        post(s, 0, FI_REPLY_CMD, 12, 0, 0, 0);
        expect(result(s, 0, "a Reply hangs") == J_E_PEER,
               "a Reply to a killed caller isn't E_PEER");
        outcome[O_PEER]++;
    } else {
        cur_kp = KP_CALL_QUEUED;
        post(c, 0, FI_CALL, FI_EP, 11, FI_NOCAP, J_FOREVER);
        jitter();
        kill(c, 0, how);
        post(s, 0, FI_RECV, FI_EP, 0, 0, 0);
        expect(result(s, 0, "a Recv with timeout 0 hangs") == J_E_TIMEOUT,
               "a killed caller's message was still there to receive");
        outcome[O_WITHDRAWN]++;
    }
    /* The server goes on to the next caller. */
    post(s, 0, FI_RECV, FI_EP, J_FOREVER, 0, 0);
    post(b, 0, FI_CALL, FI_EP, 21, FI_NOCAP, J_FOREVER);
    expect(result(s, 0, "a server's Recv hangs with a caller there") == J_OK &&
               th(s, 0)->val[0] == 2 && th(s, 0)->val[1] == 21,
           "the server didn't receive the next caller's message");
    post(s, 0, FI_REPLY_CMD, 22, 0, 0, 0);
    expect(result(s, 0, "a Reply hangs") == J_OK, "a Reply to a live caller failed");
    expect(result(b, 0, "a caller hangs though it was answered") == J_OK && th(b, 0)->val[0] == 22,
           "the next caller didn't get its answer");
    /* No-senders for the killed client's badge: when its task goes, once. */
    if (!task_level(how)) {
        ns_quiet(ns, s, FI_EP, "no-senders fired while the task holding the badge lived");
        destroy(c);
    }
    ns_fired(ns, s, FI_EP, 1);
}

/* 2. A server killed holding a reply capability. */
static void sc_server(void)
{
    struct peer *s = peer_new(0, 1, 90, 0);
    struct peer *c1 = peer_new(1, 1, 100, 0);
    struct peer *c2 = peer_new(2, 1, 100, 0);
    uint64_t er;
    uint64_t dn;
    uint64_t ep = endpoint_for(s, &er, &dn);
    hand(c1, mint(ep, J_R_SEND | J_R_GRANT, 1), FI_EP, J_R_SEND);
    hand(c2, mint(ep, J_R_SEND | J_R_GRANT, 2), FI_EP, J_R_SEND);
    need(j_cap_delete(ep), "cap_delete");

    unsigned how = below(4);
    int passive = below(3) == 0;
    int holding = 1;
    uint64_t e;
    cur_how = how;
    if (passive) {
        /* Started as docs/abi.md says: on a context of its own until its
         * starter has an answer from a ReplyRecv, then without one. */
        struct j_msg m = {0};
        post(s, 0, FI_FIRST, FI_EP, 0, 0, 0);
        expect(j_call(er, &m, BOUND_NS) == J_OK, "a passive server's start failed");
        need(j_sc_unbind(s->sc[0]), "sc_unbind");
    } else {
        post(s, 0, FI_RECV, FI_EP, J_FOREVER, 0, 0);
        begun(s, 0);
    }
    post(c1, 0, FI_CALL, FI_EP, 11, FI_NOCAP, J_FOREVER);
    if (passive || below(2)) {
        cur_kp = passive ? KP_SERVER_PASSIVE : KP_SERVER_HOLDING;
        expect(result(s, 0, "a server's Recv hangs with a caller there") == J_OK &&
                   th(s, 0)->val[0] == 1,
               "the server didn't receive its caller");
        /* A second caller queues behind (the server is busy holding). */
        post(c2, 0, FI_CALL, FI_EP, 21, FI_NOCAP, J_FOREVER);
        begun(c2, 0);
    } else {
        cur_kp = KP_SERVER_EARLY;
        holding = 0;
    }
    jitter();
    if (!holding) {
        holding = th(s, 0)->done == th(s, 0)->posted; /* it had received, then */
    }
    kill(s, 0, how);
    int c1_seen = 0;
    if (!task_level(how)) {
        if (holding) {
            expect(
                result(c1, 0, "a caller hangs after its server's thread was killed") == J_E_PEER,
                "a caller whose server thread died holding its reply capability didn't get E_PEER");
            outcome[O_PEER]++;
            c1_seen = 1;
            /* It goes on, on its own time (a loan comes back): a call on the
             * live endpoint nobody serves times out. */
            post(c1, 0, FI_CALL, FI_EP, 12, FI_NOCAP, 30 * J_MS);
            expect(result(c1, 0, "a caller can't go on after its server thread died") ==
                       J_E_TIMEOUT,
                   "a call on a live endpoint nobody receives on didn't time out");
            outcome[O_WENT_ON]++;
            if (cur_kp != KP_SERVER_EARLY) {
                expect(!finished(c2, 0, STILL_NS),
                       "a caller queued on a live endpoint woke when a server thread died");
            }
        }
        expect(quiet(dn), "a dead-name notification came while the endpoint lived");
        destroy(s); /* now the endpoint dies */
    }
    if (!c1_seen) {
        /* Received and then killed: E_PEER; still queued (only if the kill
         * came early): E_GONE when the endpoint died. */
        e = result(c1, 0, "a caller hangs after its server died");
        expect(e == J_E_PEER || (!holding && e == J_E_GONE),
               holding ? "a caller whose server died holding its reply capability didn't get E_PEER"
                       : "a caller whose server died got neither E_PEER nor E_GONE");
        outcome[e == J_E_PEER ? O_PEER : O_GONE]++;
    }
    if (cur_kp != KP_SERVER_EARLY) {
        expect(result(c2, 0, "a queued caller hangs after its endpoint died") == J_E_GONE,
               "a caller queued on an endpoint that died didn't get E_GONE");
        outcome[O_GONE]++;
    }
    dn_fired(dn);
    /* On a dead endpoint every call is E_GONE at once, as is a new
     * dead-name request. */
    struct j_msg m = {0};
    expect(j_call(er, &m, BOUND_NS) == J_E_GONE, "the root's Call on a dead endpoint isn't E_GONE");
    expect(j_request_deadname(er, mint(dn, J_R_SEND, DN_BIT)) == J_E_GONE,
           "request_deadname on a dead endpoint isn't E_GONE");
    post(c1, 0, FI_CALL, FI_EP, 13, FI_NOCAP, J_FOREVER);
    expect(result(c1, 0, "a Call on a dead endpoint hangs") == J_E_GONE,
           "a survivor's Call on a dead endpoint isn't E_GONE");
    outcome[O_GONE]++;
}

/* 3. A server killed holding a saved reply. Thread 0 receives and saves,
 * thread 1 answers through the reply object. */
static void sc_saved(void)
{
    struct peer *s = peer_new(0, 2, 90, 90);
    struct peer *c = peer_new(1, 1, 100, 0);
    uint64_t er;
    uint64_t dn;
    uint64_t ep = endpoint_for(s, &er, &dn);
    hand(c, mint(ep, J_R_SEND | J_R_GRANT, 1), FI_EP, J_R_SEND);
    need(j_cap_delete(ep), "cap_delete");
    hand(s, obj(J_REPLY), FI_REPLY, J_R_SEND);
    post(s, 0, FI_RECV, FI_EP, J_FOREVER, 0, 0);
    begun(s, 0);
    post(c, 0, FI_CALL, FI_EP, 31, FI_NOCAP, J_FOREVER);
    expect(result(s, 0, "a server's Recv hangs with a caller there") == J_OK &&
               th(s, 0)->val[0] == 1,
           "the server didn't receive its caller");

    if (below(3) == 0) {
        cur_kp = KP_SAVED_HOLD;
        cur_how = below(4);
        jitter();
        kill(s, 0, cur_how);
        expect(
            result(c, 0, "a caller hangs after its server died") == J_E_PEER,
            "a caller whose server died holding its (unsaved) reply capability didn't get E_PEER");
        outcome[O_PEER]++;
        if (task_level(cur_how)) {
            dn_fired(dn);
        } else {
            expect(quiet(dn), "a dead-name notification came while the endpoint lived");
        }
        return;
    }
    cur_kp = KP_SAVED;
    post(s, 0, FI_SAVE, FI_REPLY, 0, 0, 0);
    expect(result(s, 0, "reply_save hangs") == J_OK, "reply_save failed");
    /* Holding no reply capability now, it is free to receive again. */
    post(s, 0, FI_RECV, FI_EP, J_FOREVER, 0, 0);
    begun(s, 0);
    cur_how = below(5);
    if (cur_how == 4) {
        cur_how = K_REPLYOBJ;
    }
    jitter();
    if (cur_how == K_REPLYOBJ) {
        post(s, 1, FI_DELETE, FI_REPLY, 0, 0, 0);
        expect(result(s, 1, "cap_delete hangs") == J_OK, "deleting the reply object failed");
        count[cur_kp][K_REPLYOBJ]++;
        expect(result(c, 0, "a caller hangs after its saved reply's object was deleted") ==
                   J_E_PEER,
               "a caller whose saved reply's object was deleted didn't get E_PEER");
        outcome[O_PEER]++;
        s->parked[0] = 1; /* still in its Recv, on a live endpoint */
        expect(quiet(dn), "a dead-name notification came while the endpoint lived");
    } else if (!task_level(cur_how)) {
        kill(s, 0, cur_how);
        expect(!finished(c, 0, STILL_NS), "a caller woke when the thread that saved its reply died "
                                          "(the reply object outlives it)");
        expect(quiet(dn), "a dead-name notification came while the endpoint lived");
        post(s, 1, FI_REPLY_SAVED, FI_REPLY, 32, 0, 0);
        expect(result(s, 1, "reply_saved hangs") == J_OK,
               "reply_saved from the other thread failed");
        expect(result(c, 0, "a caller hangs though its saved reply was answered") == J_OK &&
                   th(c, 0)->val[0] == 32,
               "a saved reply answered by another thread didn't arrive");
        outcome[O_SAVED_OK]++;
    } else {
        kill(s, 0, cur_how);
        expect(result(c, 0, "a caller hangs after its server's task died") == J_E_PEER,
               "a caller whose saved reply's task died didn't get E_PEER");
        outcome[O_PEER]++;
        dn_fired(dn);
    }
}

/* 4. A capability in a message when its sender or receiver dies, or when
 * it is deleted on the way. The capability is a badged SEND to an endpoint
 * of the root's, whose no-senders the root watches; the client (thread 0
 * calls, thread 1 helps) holds its only copy. */
static void sc_transfer(void)
{
    struct peer *s = peer_new(0, 1, 90, 0);
    struct peer *c = peer_new(1, 2, 100, 100);
    uint64_t er;
    uint64_t dn;
    uint64_t ep = endpoint_for(s, &er, &dn);
    hand(c, mint(ep, J_R_SEND | J_R_GRANT, 1), FI_EP, J_R_SEND);
    need(j_cap_delete(ep), "cap_delete");
    uint64_t e2 = obj(J_ENDPOINT);
    uint64_t ns = obj(J_NOTIFICATION);
    need(j_bind_nosenders(e2, mint(ns, J_R_SEND, NS_BIT)), "bind_nosenders");
    uint64_t xb = 0x100u + round_no;
    hand(c, mint(e2, J_R_SEND | J_R_GRANT, xb), FI_X, J_R_SEND | J_R_GRANT);
    post(s, 0, FI_RECV_SLOTS, FI_GOT, 0, 0, 0);
    expect(result(s, 0, "recv_slots hangs") == J_OK, "recv_slots failed");

    unsigned how = below(4);
    uint64_t e;
    cur_how = how;
    switch (below(4)) {
    case 0: /* the sender killed, its Call (and the capability) still queued */
        cur_kp = KP_XFER_QUEUED;
        post(c, 0, FI_CALL, FI_EP, 41, FI_X, J_FOREVER);
        jitter();
        kill(c, 0, how);
        post(s, 0, FI_RECV, FI_EP, 0, 0, 0);
        expect(result(s, 0, "a Recv with timeout 0 hangs") == J_E_TIMEOUT,
               "a killed sender's message (with a capability) was still there to receive");
        outcome[O_WITHDRAWN]++;
        if (!task_level(how)) {
            ns_quiet(ns, NULL, e2, "no-senders fired while the task holding the capability lived");
            destroy(c);
        }
        ns_fired(ns, NULL, e2, xb);
        break;
    case 1: /* the sender killed after its capability was delivered */
        cur_kp = KP_XFER_DELIVERED;
        post(s, 0, FI_RECV, FI_EP, J_FOREVER, 0, 0);
        begun(s, 0);
        post(c, 0, FI_CALL, FI_EP, 41, FI_X, J_FOREVER);
        e = result(s, 0, "a server's Recv hangs with a caller there");
        expect(e == J_OK && th(s, 0)->val[0] == 1 && J_CAPS_COUNT(th(s, 0)->val[2]) == 1 &&
                   J_CAPS_SLOT(th(s, 0)->val[2], 0) == FI_GOT,
               "a capability passed in a Call didn't land in the receive slot");
        jitter();
        kill(c, 0, how);
        post(s, 0, FI_REPLY_CMD, 42, 0, 0, 0);
        expect(result(s, 0, "a Reply hangs") == J_E_PEER,
               "a Reply to a killed caller isn't E_PEER");
        outcome[O_PEER]++;
        ns_quiet(ns, NULL, e2, "no-senders fired though the receiver holds a copy of the badge");
        /* The badge's last sender is whichever copy goes second. */
        if (task_level(how) || below(2)) {
            post(s, 0, FI_DELETE, FI_GOT, 0, 0, 0);
            expect(result(s, 0, "cap_delete hangs") == J_OK, "deleting the delivered copy failed");
            if (!task_level(how)) {
                ns_quiet(ns, NULL, e2,
                         "no-senders fired while the sender's task still held the badge");
                destroy(c);
            }
        } else {
            destroy(c);
            ns_quiet(ns, NULL, e2,
                     "no-senders fired though the receiver holds a copy of the badge");
            post(s, 0, FI_DELETE, FI_GOT, 0, 0, 0);
            expect(result(s, 0, "cap_delete hangs") == J_OK, "deleting the delivered copy failed");
        }
        ns_fired(ns, NULL, e2, xb);
        break;
    case 2: /* the receiver killed with a capability queued for it */
        cur_kp = KP_XFER_RECEIVER;
        post(c, 0, FI_CALL, FI_EP, 41, FI_X, J_FOREVER);
        begun(c, 0);
        jitter();
        kill(s, 0, how);
        if (!task_level(how)) {
            expect(!finished(c, 0, STILL_NS),
                   "a caller queued on a live endpoint woke when a server thread died");
            expect(quiet(dn), "a dead-name notification came while the endpoint lived");
            destroy(s);
        }
        expect(result(c, 0, "a queued caller hangs after its endpoint died") == J_E_GONE,
               "a caller queued (with a capability) on an endpoint that died didn't get E_GONE");
        outcome[O_GONE]++;
        dn_fired(dn);
        ns_quiet(ns, NULL, e2, "no-senders fired though the capability never left its sender");
        post(c, 1, FI_DELETE, FI_X, 0, 0, 0);
        expect(result(c, 1, "cap_delete hangs") == J_OK, "deleting the sender's capability failed");
        ns_fired(ns, NULL, e2, xb);
        break;
    default: /* the capability deleted by the sender's other thread while its Call is queued */
        cur_kp = KP_XFER_LOST;
        cur_how = K_CAPDEL;
        post(c, 0, FI_CALL, FI_EP, 41, FI_X, J_FOREVER);
        begun(c, 0);
        jitter();
        post(c, 1, FI_DELETE, FI_X, 0, 0, 0);
        expect(result(c, 1, "cap_delete hangs") == J_OK, "deleting the sender's capability failed");
        count[cur_kp][K_CAPDEL]++;
        /* A message on its way is no sender: the badge has none left. */
        ns_fired(ns, NULL, e2, xb);
        post(s, 0, FI_RECV, FI_EP, 0, 0, 0);
        e = result(s, 0, "a Recv with timeout 0 hangs");
        if (e == J_OK) {
            expect(th(s, 0)->val[0] == 1 && J_CAPS_COUNT(th(s, 0)->val[2]) == 1 &&
                       J_CAPS_SLOT(th(s, 0)->val[2], 0) == SLOT_NONE,
                   "a capability deleted after its send was delivered, or not reported as skipped");
            post(s, 0, FI_DELETE, FI_GOT, 0, 0, 0);
            expect(result(s, 0, "cap_delete hangs") == J_E_EMPTY,
                   "a skipped capability is in the receive slot");
            post(s, 0, FI_REPLY_CMD, 42, 0, 0, 0);
            expect(result(s, 0, "a Reply hangs") == J_OK, "a Reply to a live caller failed");
            expect(result(c, 0, "a caller hangs though it was answered") == J_OK &&
                       th(c, 0)->val[0] == 42,
                   "a caller whose capability was skipped didn't get its answer");
            outcome[O_SKIPPED]++;
        } else {
            /* The Call came after the delete (several CPUs): refused at its send. */
            expect(e == J_E_TIMEOUT && result(c, 0, "a Call hangs") == J_E_EMPTY,
                   "a Call with a capability deleted before it neither arrived nor was refused");
            outcome[O_REFUSED]++;
        }
        break;
    }
}

/* 5. A thread killed while signalling, or while waiting for signals. The
 * signaller holds FI_NSIG capabilities to one notification, a bit each,
 * and signals them in order; the waiter ORs together what it gets. */
static void sc_signal(void)
{
    struct peer *g = peer_new(0, 1, 100, 0);
    struct peer *w = peer_new(1, 1, 110, 0);
    uint64_t n = obj(J_NOTIFICATION);
    give(w, n, FI_N, J_R_RECV);
    for (unsigned i = 0; i < FI_NSIG; i++) {
        hand(g, mint(n, J_R_SEND | J_R_GRANT, 1ull << i), FI_SIG + i, J_R_SEND);
    }
    uint64_t stop = mint(n, J_R_SEND, FI_STOP);
    const struct fi_thread *gt = th(g, 0);
    const struct fi_thread *wt = th(w, 0);
    post(w, 0, FI_WAITS, FI_N, 0, 0, 0);
    begun(w, 0);

    uint64_t lo;
    uint64_t hi;
    unsigned how = below(4);
    cur_how = how;
    if (below(2)) {
        cur_kp = KP_SIGNALLER;
        post(g, 0, FI_SIGNALS, 0, FI_NSIG, below(4) * 2000u, 0);
        jitter();
        sleep_ns(rnd() % (3 * J_MS));
        kill(g, 0, how);
        uint64_t sent = gt->sent;
        uint64_t intent = gt->intent;
        sleep_ns(STILL_NS);
        expect(gt->sent == sent && gt->intent == intent, "a killed thread went on signalling");
        /* The one it had begun may or may not have gone. */
        lo = mask_of(sent);
        hi = intent > sent ? mask_of(intent) : lo;
    } else {
        cur_kp = KP_WAITER;
        post(g, 0, FI_SIGNALS, 0, FI_NSIG / 2u, 0, 0);
        expect(result(g, 0, "Signal hangs") == J_OK, "a Signal failed");
        for (uint64_t waited = 0; wt->got != mask_of(FI_NSIG / 2u); waited += POLL_NS) {
            expect(waited < BOUND_NS, "a waiter didn't get the signals sent to it");
            sleep_ns(POLL_NS);
        }
        sleep_ns(SETTLE_NS); /* in its Wait again */
        kill(w, 0, how);
        post(g, 0, FI_SIGNALS, FI_NSIG / 2u, FI_NSIG / 2u, 0, 0);
        expect(result(g, 0, "Signal hangs") == J_OK, "a Signal failed");
        lo = hi = mask_of(FI_NSIG);
    }
    if (!w->gone[0] && !w->task_gone) {
        need(j_signal(stop), "signal (stop)");
        expect(result(w, 0, "a waiter hangs: a signal didn't wake it") == J_OK, "a Wait failed");
    }
    uint64_t got = wt->got;
    uint64_t rest;
    uint64_t e = j_wait(n, 0, &rest);
    expect(e == J_OK || e == J_E_TIMEOUT, "the root's Wait failed");
    expect(wt->dups == 0 && (got & rest) == 0, "a signal was delivered twice");
    got |= rest;
    if (cur_kp == KP_WAITER) {
        expect(got == lo,
               "a signal was lost: sent after its waiter was killed, it didn't stay pending");
    } else {
        expect(got == lo || got == hi,
               "the signals that arrived aren't those sent before the signaller died");
    }
    outcome[got == lo ? O_SIGNALS_EXACT : O_SIGNALS_EDGE]++;
}

/* The end of a round: every thread still alive must have finished its last
 * command, or be parked in a call that may last for ever, which an abort
 * ends; then each ends itself, every task is destroyed, and nothing is
 * left. Its pages are free again from `mark` (lib/loader.h). */
static void teardown(uint32_t mark)
{
    cur_kp = NKP;
    cur_how = NK;
    for (unsigned i = 0; i < MAX_PEERS; i++) {
        struct peer *p = &peers[i];
        for (unsigned j = 0; p->live && !p->task_gone && j < p->nthreads; j++) {
            struct fi_thread *t = th(p, j);
            if (p->gone[j]) {
                continue;
            }
            if (t->done != t->posted) {
                if (!p->parked[j]) {
                    fail("a thread is left blocked at the end of its round");
                }
                need(j_thread_abort(p->thread[j]), "thread_abort");
                expect(finished(p, j, BOUND_NS), "an aborted thread didn't come back");
                expect(t->res == J_E_ABORTED,
                       "a parked thread wasn't in a call (an abort didn't end it)");
                outcome[O_ABORTED]++;
            }
            post(p, j, FI_EXIT, 0, 0, 0, 0);
        }
    }
    for (unsigned i = 0; i < MAX_PEERS; i++) {
        struct peer *p = &peers[i];
        for (unsigned j = 0; p->live && !p->task_gone && j < p->nthreads; j++) {
            for (uint64_t waited = 0; !p->gone[j] && j_thread_resume(p->thread[j]) != J_E_GONE;
                 waited += POLL_NS) {
                expect(waited < BOUND_NS, "a thread told to end didn't");
                sleep_ns(POLL_NS);
            }
        }
        if (p->live && !p->task_gone) {
            destroy(p);
        }
    }
    int more = 1;
    while (more) {
        need(j_continue(0, &more), "continue");
    }
    for (unsigned i = 0; i < MAX_PEERS; i++) {
        struct peer *p = &peers[i];
        if (!p->live) {
            continue;
        }
        uint64_t s[] = {p->task,  p->thread[0], p->thread[1], p->sc[0], p->sc[1],
                        p->go[0], p->go[1],     p->gos[0],    p->gos[1]};
        for (unsigned k = 0; k < sizeof s / sizeof s[0]; k++) {
            uint64_t e = j_cap_delete(s[k]);
            expect(e == J_OK || e == J_E_EMPTY, "cap_delete at the end of a round failed");
        }
        need(j_unmap(J_ROOT_SELF, (PEER_VA + i * J_PAGE) / J_PAGE), "unmap (peer page)");
        p->live = 0;
    }
    for (unsigned k = 0; k < nrs; k++) {
        uint64_t e = j_cap_delete(rslot[k]);
        expect(e == J_OK || e == J_E_EMPTY, "cap_delete at the end of a round failed");
    }
    nrs = 0;
    expect(ld_pages_since(mark) <= WARM_PAGES && ld_runs_since(mark) <= MAX_PEERS,
           "a round took more pages than were split off for rounds");
    ld_release(mark);
    uint64_t id;
    expect(j_task_descendant(0, &id) == J_E_EMPTY, "a task outlived its round");
}

/* The kernel's counters (kernel/sys/abi.h, struct abi_counters) after a
 * round must read what they read after the first: a thread left blocked,
 * a task or object not freed, a destruction not finished would show. */
static int same(const struct j_counters *a, const struct j_counters *b)
{
    return a->objects == b->objects && a->threads == b->threads && a->spaces == b->spaces &&
           a->leaves == b->leaves && a->mappings == b->mappings && a->pending == b->pending &&
           a->table_pool == b->table_pool && a->frames_free == b->frames_free &&
           a->table_pages == b->table_pages;
}

static void show(const char *what, const struct j_counters *c)
{
    j_puts(what);
    put(" objects ", c->objects);
    put(", threads ", c->threads);
    put(", spaces ", c->spaces);
    put(", leaves ", c->leaves);
    put(", mappings ", c->mappings);
    put(", pending ", c->pending);
    put(", page-table frames free ", c->table_pool);
    put(", lent table pages used ", c->table_pages);
    put(", frames free ", c->frames_free);
    put(", run-queue walks so far ", c->rq_walks); /* (grows: not compared) */
    j_puts("\n");
}

static void report(void)
{
    unsigned points = 0;
    for (unsigned kp = 0; kp < NKP; kp++) {
        unsigned total = 0;
        for (unsigned how = 0; how < NK; how++) {
            total += count[kp][how];
        }
        points += total != 0 ? 1u : 0u;
        put("faultinj: kill point: ", total);
        j_puts(" x ");
        j_puts(kp_name[kp]);
        const char *sep = " (";
        for (unsigned how = 0; how < NK; how++) {
            if (count[kp][how] != 0) {
                j_puts(sep);
                j_puts(how_name[how]);
                put(" ", count[kp][how]);
                sep = ", ";
            }
        }
        j_puts(total != 0 ? ")\n" : "\n");
    }
    for (unsigned o = 0; o < NO; o++) {
        put("faultinj: outcome: ", outcome[o]);
        j_puts(" x ");
        j_puts(out_name[o]);
        j_puts("\n");
    }
    put("faultinj: ", points);
    put(" of ", NKP);
    j_puts(" kill points covered\n");
    for (unsigned k = 0; k < NSC; k++) {
        j_puts("faultinj: ");
        j_puts(sc_name[k]);
        put(" ok: ", rounds[k]);
        j_puts(" rounds\n");
    }
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 1) {
        die("needs one module: fiworker");
    }
    test_init();
    for (unsigned i = 0; i < MAX_PEERS; i++) {
        struct peer *p = &peers[i];
        p->task = slot_new();
        for (unsigned j = 0; j < 2; j++) {
            p->thread[j] = slot_new();
            p->sc[j] = slot_new();
            p->go[j] = slot_new();
            p->gos[j] = slot_new();
        }
    }
    for (unsigned k = 0; k < RSLOTS; k++) {
        rslot[k] = slot_new();
    }
    /* As many pages as a round can take, split off the pool now: the frames
     * the splitting makes stay the root's, and would otherwise show in the
     * kernel's counters as growth the first time a round took more pages
     * than any before it. */
    uint32_t mark0 = ld_mark();
    for (unsigned k = 0; k < WARM_PAGES; k++) {
        (void)page_alloc();
    }
    ld_release(mark0);
    ld_warm_runs(MAX_PEERS); /* and a run of page-table pages for each peer (0021) */
    uint64_t t0 = work_tsc();
    sleep_ns(20 * J_MS);
    per_ms = (work_tsc() - t0) / 20u;
    check(per_ms != 0, "the time stamp counter doesn't advance");
    put("faultinj: seed ", FAULTINJ_SEED);
    put(", rounds ", FAULTINJ_FIRST);
    put(" to ", FAULTINJ_FIRST + FAULTINJ_ROUNDS - 1u);
    put(", ", ncpus);
    j_puts(" CPUs\n");
    struct j_counters base;
    struct j_counters now;
    for (round_no = FAULTINJ_FIRST; round_no < FAULTINJ_FIRST + FAULTINJ_ROUNDS; round_no++) {
        uint32_t mark = ld_mark();
        rng = (uint64_t)FAULTINJ_SEED * 0xd1342543de82ef95ull ^
              (round_no + 1u) * 0x9e3779b97f4a7c15ull;
        cur_sc = (unsigned)((round_no + FAULTINJ_SEED) % NSC);
        cur_kp = NKP;
        cur_how = NK;
        switch (cur_sc) {
        case SC_CALL:
            sc_call();
            break;
        case SC_SERVER:
            sc_server();
            break;
        case SC_SAVED:
            sc_saved();
            break;
        case SC_TRANSFER:
            sc_transfer();
            break;
        default:
            sc_signal();
            break;
        }
        teardown(mark);
        rounds[cur_sc]++;
        need(j_debug_counters(&now), "debug_counters");
        if (round_no == FAULTINJ_FIRST) {
            base = now;
            show("faultinj: after the first round:", &base);
        } else if (!same(&base, &now)) {
            show("faultinj: now:", &now);
            fail("the kernel's counters moved: something of the round's is left");
        }
    }
    cur_sc = NSC;
    show("faultinj: after the last round:", &now);
    report();
    j_puts("faultinj: all ok\n");
    j_exit(0);
}
