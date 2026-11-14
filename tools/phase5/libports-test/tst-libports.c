/* SPDX-License-Identifier: GPL-3.0-only */
/* tst-libports.c - libports on Joist, a server under fault injection
 * (Phase 5 step 8, the plan's Testing row: clients killed mid-call,
 * no-senders exactly once, the thread pool never exceeded). One task:
 * the servers and their clients are its threads, calling through
 * joistmig's user stubs (lpt.defs); a client is killed with
 * thread_terminate while its call is in the server, or queued for a
 * thread. Seeded (argv[1], default 1) and replayable: the seed picks the
 * calls' arguments, the work's lengths and the order of the kill rounds.
 *
 * Checked: answers right in both loops; the multithreaded loop's threads
 * never more than its pool (3), and more than one used; every object made
 * cleaned exactly once, after its last send right went, with get-and-drop
 * racing the no-senders thread (a firing taken while a right is being
 * handed out must be ignored); a destroyed port's requests answered
 * EOPNOTSUPP; an RPC interrupted (EINTR); a killed client's server thread
 * goes on serving (its reply fails, E_PEER, and is dropped), and a killed
 * queued client is never served; and every thread the test makes starts,
 * with its argument (a kernel bug after a kill: spawn). Each part prints a
 * line as it ends (stdout a line at a time). Exit 0 if all hold. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <hurd/ports.h>
#include <mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "lpt_types.h"
#include "lpt_U.h"
#include "lpt_S.h"

int lpt_server (struct joist_mig_msg *in, struct joist_mig_msg *out);

#define POOL 3
#define IDS 256

struct lpt_obj
{
  struct port_info pi;
  int id;
};

static struct port_class *objc;
static struct port_bucket *b1, *b2;
static int failures;

#define CHECK(c)                                                            \
  do                                                                        \
    if (!(c))                                                               \
      {                                                                     \
	printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
	__atomic_add_fetch (&failures, 1, __ATOMIC_SEQ_CST);                \
      }                                                                     \
  while (0)

static uint64_t rs;
static uint32_t
rnd (void)
{
  rs ^= rs << 13;
  rs ^= rs >> 7;
  rs ^= rs << 17;
  return (uint32_t) rs;
}

static uint64_t
now_ms (void)
{
  struct timespec t;
  clock_gettime (CLOCK_MONOTONIC, &t);
  return (uint64_t) t.tv_sec * 1000 + (uint64_t) t.tv_nsec / 1000000;
}

static void
sleep_ms (int ms)
{
  struct timespec t = { ms / 1000, (long) (ms % 1000) * 1000000 };
  nanosleep (&t, NULL);
}

/* Wait (at most LIMIT ms) for COND.  */
#define WAIT_FOR(cond, limit)                                               \
  ({                                                                        \
    uint64_t _end = now_ms () + (limit);                                    \
    while (!(cond) && now_ms () < _end)                                     \
      sleep_ms (2);                                                         \
    (cond);                                                                 \
  })

/* ---- the server's side ---- */

static int made[IDS], cleaned[IDS];
static mach_port_t names[IDS];
static pthread_mutex_t names_lock = PTHREAD_MUTEX_INITIALIZER;
static int cur, peak, arrived, served, hold_open, hooks;

lpt_obj_t
lpt_in (mach_port_t name)
{
  return ports_lookup_port (NULL, name, objc);
}

lpt_obj_t
lpt_in_payload (unsigned long payload)
{
  return ports_lookup_payload (NULL, payload, objc);
}

void
lpt_done (lpt_obj_t o)
{
  if (o != NULL)
    ports_port_deref (o);
}

static void
obj_clean (void *p)
{
  struct lpt_obj *o = p;
  if (o->id >= 0 && o->id < IDS)
    __atomic_add_fetch (&cleaned[o->id], 1, __ATOMIC_SEQ_CST);
}

kern_return_t
S_lpt_add (lpt_obj_t o, int a, int b, int *c)
{
  if (o == NULL)
    return EOPNOTSUPP;
  *c = a + b;
  return 0;
}

kern_return_t
S_lpt_slow (lpt_obj_t o, int ms, int *pk)
{
  if (o == NULL)
    return EOPNOTSUPP;
  int n = __atomic_add_fetch (&cur, 1, __ATOMIC_SEQ_CST);
  int p = __atomic_load_n (&peak, __ATOMIC_SEQ_CST);
  while (n > p && !__atomic_compare_exchange_n (&peak, &p, n, 0,
						__ATOMIC_SEQ_CST,
						__ATOMIC_SEQ_CST))
    ;
  __atomic_add_fetch (&arrived, 1, __ATOMIC_SEQ_CST);
  kern_return_t r = 0;
  if (ms < 0)
    WAIT_FOR (__atomic_load_n (&hold_open, __ATOMIC_SEQ_CST), 10000);
  else
    for (int i = 0; i < ms; i++)
      {
	if (ports_self_interrupted ())
	  {
	    r = EINTR;
	    break;
	  }
	sleep_ms (1);
      }
  __atomic_sub_fetch (&cur, 1, __ATOMIC_SEQ_CST);
  __atomic_add_fetch (&served, 1, __ATOMIC_SEQ_CST);
  *pk = __atomic_load_n (&peak, __ATOMIC_SEQ_CST);
  return r;
}

static kern_return_t
make_obj (struct port_bucket *b, int id, mach_port_t *right)
{
  struct lpt_obj *o;
  error_t err = ports_create_port (objc, b, sizeof *o, &o);
  if (err)
    return err;
  o->id = id;
  *right = ports_get_right (o);
  pthread_mutex_lock (&names_lock);
  names[id] = o->pi.port_right;
  made[id]++;
  pthread_mutex_unlock (&names_lock);
  ports_port_deref (o);
  return 0;
}

kern_return_t
S_lpt_make (lpt_obj_t o, int id, mach_port_t *obj)
{
  if (o == NULL)
    return EOPNOTSUPP;
  if (id < 0 || id >= IDS)
    return EINVAL;
  return make_obj (o->pi.bucket, id, obj);
}

static struct lpt_obj *
find (int id)
{
  pthread_mutex_lock (&names_lock);
  mach_port_t n = names[id];
  pthread_mutex_unlock (&names_lock);
  struct lpt_obj *p = n ? ports_lookup_port (NULL, n, objc) : NULL;
  if (p != NULL && p->id != id)
    {
      ports_port_deref (p);
      p = NULL;
    }
  return p;
}

kern_return_t
S_lpt_get (lpt_obj_t o, int id, mach_port_t *obj)
{
  if (o == NULL)
    return EOPNOTSUPP;
  if (id < 0 || id >= IDS)
    return EINVAL;
  struct lpt_obj *p = find (id);
  if (p == NULL)
    return ENOENT;
  *obj = ports_get_right (p);
  ports_port_deref (p);
  return 0;
}

static void *
one_thread (void *arg)
{
  ports_manage_port_operations_one_thread (arg, lpt_server, 0);
  return NULL;
}

static void
hook (void)
{
  __atomic_add_fetch (&hooks, 1, __ATOMIC_SEQ_CST);
}

static void *
multithread (void *arg)
{
  ports_manage_port_operations_multithread (arg, lpt_server, 0, 0, hook);
  return NULL;
}

/* ---- clients ---- */

/* A thread of the test's, started through a hand-off as well as its
   argument, and checked: its failure to be made or to start is counted (a
   client never started would otherwise look like a call never served).

   The hand-off is for the kernel's bug found in Phase 5 step 8
   (kernel/sys/abi.c): a thread killed while blocked in a system call
   leaves its slot's call "pending", and the next thread spawned into that
   slot has its first registers overwritten by that call's outputs (RDX,
   its start argument, becomes 0).  Such a start is a failure, named; the
   thread goes on with the argument handed off, so the parts after a kill
   still test libports.  Only main spawns, one at a time.  */
static struct
{
  void *(*fn) (void *);
  void *arg;
  int taken;
} handoff;

static void *
trampoline (void *a)
{
  void *(*fn) (void *) = handoff.fn;
  void *arg = handoff.arg;
  __atomic_store_n (&handoff.taken, 1, __ATOMIC_SEQ_CST);
  if (a != arg)
    {
      printf ("FAIL kernel: a new thread's start argument lost (%p, not %p):"
	      " a killed thread's pending call (sys/abi.c)\n", a, arg);
      __atomic_add_fetch (&failures, 1, __ATOMIC_SEQ_CST);
    }
  return fn (arg);
}

static int
spawn (pthread_t *t, void *(*fn) (void *), void *arg)
{
  handoff.fn = fn;
  handoff.arg = arg;
  __atomic_store_n (&handoff.taken, 0, __ATOMIC_SEQ_CST);
  int e = pthread_create (t, NULL, trampoline, arg);
  if (e != 0)
    {
      printf ("FAIL pthread_create: %d\n", e);
      __atomic_add_fetch (&failures, 1, __ATOMIC_SEQ_CST);
      return e;
    }
  if (!WAIT_FOR (__atomic_load_n (&handoff.taken, __ATOMIC_SEQ_CST), 5000))
    {
      printf ("FAIL a thread made never started\n");
      __atomic_add_fetch (&failures, 1, __ATOMIC_SEQ_CST);
      return EAGAIN;
    }
  return 0;
}

static mach_port_t s1, s2;	/* send rights to the two root objects */

struct client
{
  pthread_t t;
  mach_port_t self;		/* its thread, for thread_terminate */
  int ms, n, bad;
  int together;			/* wait for GO */
  kern_return_t r;
};

/* A part's clients made one at a time (spawn) start their calls together
   once all are made: GO, set by the part.  */
static int go;

static void
wait_go (void)
{
  WAIT_FOR (__atomic_load_n (&go, __ATOMIC_SEQ_CST), 5000);
}

static void *
slow_client (void *arg)
{
  struct client *c = arg;
  __atomic_store_n (&c->self, mach_thread_self (), __ATOMIC_SEQ_CST);
  if (c->together)
    wait_go ();
  for (int i = 0; i < c->n; i++)
    {
      int pk;
      kern_return_t r = lpt_slow (s2, c->ms, &pk);
      if (r != 0)
	c->bad++;
      c->r = r;
    }
  return NULL;
}

static int race_bad, race_gone;
#ifndef RACE_ROOT
# define RACE_ROOT s1
#endif

/* Get a right to object 100, use it, drop it; remake it if it's gone.  */
static void *
race_client (void *arg)
{
  struct client *c = arg;
  wait_go ();
  for (int i = 0; i < c->n; i++)
    {
      mach_port_t n = MACH_PORT_NULL;
      kern_return_t r = lpt_get (RACE_ROOT, 100, &n);
      if (r == ENOENT)
	{
	  __atomic_add_fetch (&race_gone, 1, __ATOMIC_SEQ_CST);
	  r = lpt_make (RACE_ROOT, 100, &n);
	}
      if (r != 0)
	{
	  printf ("race: get/make %d\n", r);
	  __atomic_add_fetch (&race_bad, 1, __ATOMIC_SEQ_CST);
	  continue;
	}
      int sum;
      r = lpt_add (n, i, 1, &sum);
      if (r != 0 || sum != i + 1)
	{
	  /* A right handed out, its port freed: a stale firing acted on.  */
	  printf ("race: a held right refused (%d) i %d sum %d n %u self %u\n", r, i, sum, n, mach_thread_self ());
	  __atomic_add_fetch (&race_bad, 1, __ATOMIC_SEQ_CST);
	}
      if (rnd () % 4 == 0)
	sched_yield ();
      mach_port_deallocate (mach_task_self (), n);
    }
  return NULL;
}

static int
all_cleaned (int from, int to)
{
  for (int id = from; id < to; id++)
    if (__atomic_load_n (&cleaned[id], __ATOMIC_SEQ_CST)
	!= __atomic_load_n (&made[id], __ATOMIC_SEQ_CST))
      return 0;
  return 1;
}

/* ---- the parts ---- */

static void
part_add (void)
{
  for (int i = 0; i < 2000; i++)
    {
      int a = (int) (rnd () % 100000), b = (int) (rnd () % 100000), c = -1;
      kern_return_t r = lpt_add (i % 2 ? s1 : s2, a, b, &c);
      if (r != 0 || c != a + b)
	{
	  printf ("add %d: %d, %d + %d = %d\n", i, r, a, b, c);
	  failures++;
	  return;
	}
    }
  printf ("add: 2000 answers right, both loops\n");
}

static void
part_pool (void)
{
  struct client c[6];
  memset (c, 0, sizeof c);
  __atomic_store_n (&go, 0, __ATOMIC_SEQ_CST);
  for (int i = 0; i < 6; i++)
    {
      c[i].ms = 5 + (int) (rnd () % 20);
      c[i].n = 4;
      c[i].together = 1;
      spawn (&c[i].t, slow_client, &c[i]);
    }
  __atomic_store_n (&go, 1, __ATOMIC_SEQ_CST);
  int bad = 0;
  for (int i = 0; i < 6; i++)
    {
      pthread_join (c[i].t, NULL);
      bad += c[i].bad;
    }
  int pk = __atomic_load_n (&peak, __ATOMIC_SEQ_CST);
  int h = __atomic_load_n (&hooks, __ATOMIC_SEQ_CST);
  printf ("pool: 24 calls by 6 clients, %d refused; at most %d at once, %d threads (pool %d)\n",
	  bad, pk, h, POOL);
  CHECK (bad == 0);
  CHECK (pk <= POOL && h <= POOL);
  CHECK (pk >= 2);
}

static void
part_nosenders (void)
{
  mach_port_t n[40];
  for (int i = 0; i < 40; i++)
    {
      CHECK (lpt_make (i % 2 ? s1 : s2, i, &n[i]) == 0);
      int c;
      CHECK (lpt_add (n[i], i, i, &c) == 0 && c == 2 * i);
    }
  CHECK (!all_cleaned (0, 40));
  /* A second right to some, from the server: no-senders waits for both. */
  mach_port_t again[10];
  for (int i = 0; i < 10; i++)
    CHECK (lpt_get (s1, i, &again[i]) == 0);
  for (int i = 0; i < 40; i++)
    mach_port_deallocate (mach_task_self (), n[i]);
  CHECK (WAIT_FOR (all_cleaned (10, 40), 5000));
  sleep_ms (100);
  for (int i = 0; i < 10; i++)
    CHECK (cleaned[i] == 0);
  for (int i = 0; i < 10; i++)
    mach_port_deallocate (mach_task_self (), again[i]);
  CHECK (WAIT_FOR (all_cleaned (0, 40), 5000));
  sleep_ms (200);
  int twice = 0;
  for (int i = 0; i < 40; i++)
    twice += cleaned[i] != 1;
  printf ("no-senders: 40 objects, each cleaned once after its last right (%d not)\n",
	  twice);
  CHECK (twice == 0);
}

static void
part_race (void)
{
  struct client c[3];
  memset (c, 0, sizeof c);
  __atomic_store_n (&go, 0, __ATOMIC_SEQ_CST);
  for (int i = 0; i < 3; i++)
    {
      c[i].n = 300;
      spawn (&c[i].t, race_client, &c[i]);
    }
  __atomic_store_n (&go, 1, __ATOMIC_SEQ_CST);
  for (int i = 0; i < 3; i++)
    pthread_join (c[i].t, NULL);
  CHECK (WAIT_FOR (all_cleaned (100, 101), 5000));
  sleep_ms (100);
  printf ("race: 900 get/use/drop, %d objects made and cleaned, %d remade, %d refused\n",
	  made[100], race_gone, race_bad);
  CHECK (race_bad == 0);
  CHECK (cleaned[100] == made[100]);
}

static void
part_destroyed (void)
{
  mach_port_t n;
  CHECK (lpt_make (s1, 200, &n) == 0);
  struct lpt_obj *o = find (200);
  CHECK (o != NULL);
  if (o == NULL)
    return;
  ports_destroy_right (o);
  ports_port_deref (o);
  int c;
  kern_return_t r = lpt_add (n, 1, 2, &c);
  printf ("destroyed: a held right's call answered %d\n", r);
  CHECK (r == EOPNOTSUPP);
  mach_port_deallocate (mach_task_self (), n);
  CHECK (WAIT_FOR (all_cleaned (200, 201), 2000));
}

static void
part_interrupt (void)
{
  struct client c = { .ms = 3000, .n = 1 };
  int a0 = __atomic_load_n (&arrived, __ATOMIC_SEQ_CST);
  uint64_t t0 = now_ms ();
  spawn (&c.t, slow_client, &c);
  CHECK (WAIT_FOR (__atomic_load_n (&arrived, __ATOMIC_SEQ_CST) > a0, 2000));
  pthread_mutex_lock (&names_lock);
  mach_port_t rn = names[IDS - 1];
  pthread_mutex_unlock (&names_lock);
  struct lpt_obj *r2 = ports_lookup_port (b2, rn, objc);
  CHECK (r2 != NULL);
  if (r2 != NULL)
    {
      ports_interrupt_rpcs (r2);
      ports_port_deref (r2);
    }
  pthread_join (c.t, NULL);
  uint64_t took = now_ms () - t0;
  printf ("interrupt: answered %d after %llu ms\n", c.r,
	  (unsigned long long) took);
  CHECK (c.r == EINTR && took < 2500);
}

/* A client killed while the server works on its call: the server's reply
   fails and is dropped, and it goes on serving.  */
static void
kill_in_server (void)
{
  __atomic_store_n (&hold_open, 0, __ATOMIC_SEQ_CST);
  int a0 = __atomic_load_n (&arrived, __ATOMIC_SEQ_CST);
  int s0 = __atomic_load_n (&served, __ATOMIC_SEQ_CST);
  struct client *c = calloc (1, sizeof *c);	/* (never joined: leaked) */
  c->ms = -1;
  c->n = 1;
  spawn (&c->t, slow_client, c);
  CHECK (WAIT_FOR (__atomic_load_n (&arrived, __ATOMIC_SEQ_CST) > a0, 2000));
  CHECK (thread_terminate (__atomic_load_n (&c->self, __ATOMIC_SEQ_CST)) == 0);
  sleep_ms ((int) (rnd () % 5));
  __atomic_store_n (&hold_open, 1, __ATOMIC_SEQ_CST);
  CHECK (WAIT_FOR (__atomic_load_n (&served, __ATOMIC_SEQ_CST) > s0, 2000));
  int x;
  CHECK (lpt_add (s2, 2, 3, &x) == 0 && x == 5);
}

/* The pool full of held calls and one more client queued in the kernel,
   killed there: never served; the held ones answered.  */
static void
kill_queued (void)
{
  __atomic_store_n (&hold_open, 0, __ATOMIC_SEQ_CST);
  int a0 = __atomic_load_n (&arrived, __ATOMIC_SEQ_CST);
  struct client h[POOL];
  memset (h, 0, sizeof h);
  for (int i = 0; i < POOL; i++)
    {
      h[i].ms = -1;
      h[i].n = 1;
      spawn (&h[i].t, slow_client, &h[i]);
    }
  CHECK (WAIT_FOR (__atomic_load_n (&arrived, __ATOMIC_SEQ_CST) == a0 + POOL,
		   2000));
  struct client *v = calloc (1, sizeof *v);	/* (never joined) */
  v->ms = 0;
  v->n = 1;
  spawn (&v->t, slow_client, v);
  WAIT_FOR (__atomic_load_n (&v->self, __ATOMIC_SEQ_CST) != 0, 2000);
  sleep_ms (30 + (int) (rnd () % 20));	/* in its call, queued */
  CHECK (thread_terminate (__atomic_load_n (&v->self, __ATOMIC_SEQ_CST)) == 0);
  __atomic_store_n (&hold_open, 1, __ATOMIC_SEQ_CST);
  int bad = 0;
  for (int i = 0; i < POOL; i++)
    {
      pthread_join (h[i].t, NULL);
      bad += h[i].bad;
    }
  sleep_ms (50);
  CHECK (bad == 0);
  CHECK (__atomic_load_n (&arrived, __ATOMIC_SEQ_CST) == a0 + POOL);
  int x;
  CHECK (lpt_add (s2, 4, 5, &x) == 0 && x == 9);
}

static void
part_kills (void)
{
  int k1 = 0, k2 = 0;
  for (int i = 0; i < 6; i++)
    if (rnd () % 2)
      {
	kill_in_server ();
	k1++;
      }
    else
      {
	kill_queued ();
	k2++;
      }
  printf ("kills: %d clients killed in the server's work, %d queued; served on, pool %d of %d\n",
	  k1, k2, __atomic_load_n (&hooks, __ATOMIC_SEQ_CST), POOL);
  CHECK (__atomic_load_n (&hooks, __ATOMIC_SEQ_CST) <= POOL);
}

int
main (int argc, char **argv)
{
  unsigned seed = argc > 1 ? (unsigned) atoi (argv[1]) : 1;
  rs = 0x9e3779b97f4a7c15ull * (seed + 1);
  /* A line at a time: a run that hangs still shows the parts it ended
     (the console isn't a terminal, so stdout would be fully buffered).  */
  setvbuf (stdout, NULL, _IOLBF, 0);
  printf ("tst-libports: seed %u\n", seed);
  objc = ports_create_class (obj_clean, NULL);
  b1 = ports_create_bucket ();
  b2 = ports_create_bucket ();
  if (objc == NULL || b1 == NULL || b2 == NULL)
    {
      printf ("FAIL: no class or bucket\n");
      return 1;
    }
  ports_set_pool_size (b2, POOL);
  /* The roots: ids IDS-2 (b1) and IDS-1 (b2), with send rights of the
     test's own.  */
  struct lpt_obj *r1, *r2;
  if (ports_create_port (objc, b1, sizeof *r1, &r1) != 0
      || ports_create_port (objc, b2, sizeof *r2, &r2) != 0)
    {
      printf ("FAIL: no root\n");
      return 1;
    }
  r1->id = IDS - 2;
  r2->id = IDS - 1;
  names[IDS - 2] = r1->pi.port_right;
  names[IDS - 1] = r2->pi.port_right;
  s1 = ports_get_send_right (r1);
  s2 = ports_get_send_right (r2);
  pthread_t t1, t2;
  pthread_create (&t1, NULL, one_thread, b1);
  pthread_create (&t2, NULL, multithread, b2);

  part_add ();
  part_pool ();
  part_nosenders ();
  part_race ();
  part_destroyed ();
  part_interrupt ();
  part_kills ();

  printf ("tst-libports: %s (%d failed)\n", failures ? "FAIL" : "PASS",
	  failures);
  fflush (stdout);
  _exit (failures ? 1 : 0);
}
