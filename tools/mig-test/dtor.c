/* SPDX-License-Identifier: GPL-3.0-only */
/* dtor.c - joistmig's servers call MIG's destructors (tools/mig-test,
 * run.sh's dtor()): each translated argument (dtor.defs: the request port
 * by its badge, a port argument by its name) is destroyed once after the
 * routine, with the object the routine got, whether it succeeded or
 * failed; and an argument the request lacks is refused before any
 * translation. */
#include "d/dtor_S.h" /* joistmig's server header: the routines' types */

#include <joist_mig.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


struct dtor_obj {
    uintptr_t key;
    int live; /* translations not yet destroyed */
};
static struct dtor_obj objs[8];
static unsigned failed, translated, destroyed;

static struct dtor_obj *obj_of(uintptr_t key)
{
    return &objs[key % 8u];
}

dtor_obj_t dtor_in(mach_port_t name)
{
    translated++;
    struct dtor_obj *o = obj_of(name);
    o->live++;
    return o;
}

dtor_obj_t dtor_in_payload(uintptr_t payload)
{
    return dtor_in((mach_port_t)payload);
}

void dtor_done(dtor_obj_t o)
{
    destroyed++;
    if (o->live <= 0) {
        printf("dtor: an object destroyed more often than translated\n");
        failed++;
    }
    o->live--;
}

/* The server routines: their translated arguments (dtor_S.h). */
static dtor_obj_t seen_target, seen_other;

kern_return_t dtor_ok(dtor_obj_t target, dtor_obj_t other, int x)
{
    seen_target = target;
    seen_other = other;
    return x == 42 ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}

kern_return_t dtor_fail(dtor_obj_t target, dtor_obj_t other)
{
    seen_target = target;
    seen_other = other;
    return KERN_FAILURE;
}

/* dtor_read's memory (a `dealloc[]` result): which 0, the routine's own
 * buffer, the flag left clear (never to be freed); 1, memory it allocated,
 * the flag set (freed once, after the reply); 2, the stub's own buffer,
 * written in place (never freed). The stub frees through
 * joist_mig_deallocate, counted here. */
static char own[16] = "own buffer";
static void *given, *freed;
static unsigned frees;

void *joist_mig_allocate(size_t n)
{
    return malloc(n != 0 ? n : 1);
}

void joist_mig_deallocate(void *p, size_t n)
{
    (void)n;
    frees++;
    freed = p;
    free(p);
}

kern_return_t dtor_read(dtor_obj_t target, int which, char **data, mach_msg_type_number_t *dataCnt,
                        boolean_t *dataDealloc)
{
    (void)target;
    if (which == 0) {
        *data = own;
        *dataCnt = sizeof own;
        *dataDealloc = 0;
    } else if (which == 1) {
        given = joist_mig_allocate(8);
        memcpy(given, "fresh!!", 8);
        *data = given;
        *dataCnt = 8;
        *dataDealloc = 1;
    } else {
        memcpy(*data, "stub", 5);
        *dataCnt = 5;
    }
    return KERN_SUCCESS;
}

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("dtor: %s\n", what);
        failed++;
    }
}

/* A request for routine id with the request badge, one capability (name
 * `other`, or none) and n ints of body (a body's length is in bytes). */
static void request(uint32_t id, uint64_t badge, int with_cap, uint32_t other, const uint64_t *w,
                    unsigned n, struct joist_mig_msg *out)
{
    static uint64_t inbuf[JOIST_MIG_BUF_WORDS], outbuf[JOIST_MIG_BUF_WORDS];
    struct joist_mig_msg in;
    memset(&in, 0, sizeof in);
    in.buf = inbuf;
    in.badge = badge;
    in.ncaps = with_cap ? 1u : 0u;
    in.cap[0] = other;
    in.r[0] = id | (uint64_t)(n * sizeof(int)) << 32;
    for (unsigned i = 0; i < n && i < 3u; i++) {
        in.r[1 + i] = w[i];
    }
    memset(out, 0, sizeof *out);
    out->buf = outbuf;
    (void)dtor_server(&in, out);
}

int main(void)
{
    struct joist_mig_msg out;
    uint64_t x = 42;
    /* Succeeds: both translated, the routine sees them, both destroyed. */
    request(7000, 3, 1, 5, &x, 1, &out);
    check((uint32_t)out.r[0] == 0, "dtor_ok didn't succeed");
    check(seen_target == obj_of(3) && seen_other == obj_of(5), "the routine got other objects");
    check(translated == 2 && destroyed == 2 && objs[3].live == 0 && objs[5].live == 0,
          "a successful routine's translations not each destroyed once");
    /* Fails in the routine: destroyed all the same. */
    x = 7;
    request(7000, 2, 1, 6, &x, 1, &out);
    check((uint32_t)out.r[0] == (uint32_t)KERN_INVALID_ARGUMENT, "dtor_ok's refusal lost");
    request(7001, 1, 1, 4, NULL, 0, &out);
    check((uint32_t)out.r[0] == (uint32_t)KERN_FAILURE, "dtor_fail's result lost");
    check(translated == 6 && destroyed == 6, "a failed routine's translations not destroyed");
    /* Refused before the routine (no capability): nothing translated. */
    request(7001, 1, 0, 0, NULL, 0, &out);
    check((uint32_t)out.r[0] == (uint32_t)JOIST_MIG_BAD_ARGUMENTS, "a missing capability not refused");
    check(translated == 6 && destroyed == 6, "a refused request translated something");
    /* dealloc[]: freed exactly when the routine says so, and only then. */
    uint64_t w = 0;
    request(7002, 1, 0, 0, &w, 1, &out);
    check((uint32_t)out.r[0] == 0 && frees == 0, "the routine's own buffer freed (dealloc[] clear)");
    w = 1;
    request(7002, 1, 0, 0, &w, 1, &out);
    check((uint32_t)out.r[0] == 0 && frees == 1 && freed == given,
          "allocated memory not freed once (dealloc[] set)");
    w = 2;
    request(7002, 1, 0, 0, &w, 1, &out);
    check((uint32_t)out.r[0] == 0 && frees == 1, "the stub's own buffer freed");
    for (unsigned i = 0; i < 8u; i++) {
        check(objs[i].live == 0, "an object left translated");
    }
    printf("dtor: %u translated, %u destroyed; %u failed\n", translated, destroyed, failed);
    return failed != 0;
}
