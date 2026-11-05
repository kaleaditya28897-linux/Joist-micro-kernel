/* Mach port names on Joist's capability slots (Phase 5 step 7, items 2
   and 3 of the design; Joist decision 0005).
   Copyright (C) 2026 Aditya Kale.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

/* A Mach port name is a slot of the task's capability space.  The table
   here says what each name is, and counts its user references, which the
   kernel doesn't (Mach's urefs: a send right's capability goes when they
   reach 0).

   - A **send right**: its slot holds a capability with SEND (and GRANT).
   - A **port set**: its slot holds an endpoint, every right (0005: a set
     is one endpoint, and its ports are badges on it).
   - A **receive right** is made lazily (0005: membership is fixed before
     any send right exists).  Its badge is its protected payload if one is
     set, otherwise a counter's next value, fixed by its first send right.
     A port in a set is that badge on the set's endpoint; a port in none
     gets an endpoint of its own, in a name of the table's (HIDDEN).  Its
     slot holds a send capability with that badge only while the name has
     send references: the receiver's own copy would otherwise count as a
     sender, and no-senders would never fire.  Once it has a badge, its
     set and payload no longer change (KERN_INVALID_RIGHT, as 0005 says).
   - A **dead name** holds nothing.

   Where Joist differs from Mach, by construction:
   - two send rights to one port that arrive separately get two names (the
     kernel can't say two capabilities name one object);
   - destroying a receive right in a set doesn't revoke the badged rights
     other tasks hold: their messages still reach the set, with a badge its
     server no longer knows;
   - a send right whose port dies stays a send right: its calls fail.

   The kernel's calls are the environment's (below); every check is made
   before any of them, and the table changes only after they succeed, so
   a refused call changes nothing.  Bounded: one table, a slot per name;
   finding a free name scans it once.  */

#ifndef _JOIST_NAMES_H
#define _JOIST_NAMES_H 1

#include <stdint.h>

/* The table's size: the space's (Joist: 64 leaves of 128 slots).  */
#ifndef JN_LEAF
# define JN_LEAF 128u
#endif
#ifndef JN_LEAVES
# define JN_LEAVES 64u
#endif
#define JN_NAMES (JN_LEAF * JN_LEAVES)

/* What the table needs from its environment: the kernel's calls on the
   caller's own space (sysdeps/joist; the model in the host tests).  0, or
   the kernel's error (its E_NOMEM is JN_KERN_NOMEM).  */
#define JN_KERN_NOMEM 5
extern int __joist_names_endpoint (uint32_t dst);
extern int __joist_names_mint (uint32_t src, uint32_t dst, uint64_t badge);
extern int __joist_names_derive (uint32_t src, uint32_t dst);
extern int __joist_names_delete (uint32_t slot);
extern int __joist_names_grow (uint32_t leaf);
/* A leaf grown for a call that then failed, given back (it is empty).  */
extern int __joist_names_shrink (uint32_t leaf);

enum jn_kind
{
  JN_FREE,
  JN_FIXED,			/* given at start-up: no references counted */
  JN_SEND,
  JN_RECEIVE,
  JN_PORTSET,
  JN_DEAD,
  JN_HIDDEN,			/* a receive right's own endpoint */
  JN_LANDING			/* where a capability may arrive */
};

struct jn_entry
{
  uint8_t kind;
  uint8_t minted;		/* a receive right: its badge is fixed */
  uint16_t refs;		/* send (or dead-name) references */
  uint32_t set;			/* a receive right: its set's name, or 0 */
  uint32_t ep;			/* a receive right: its own endpoint's, or 0 */
  uint64_t badge;		/* a receive right: payload, then its badge */
};

struct jn_table
{
  struct jn_entry e[JN_NAMES];
  uint64_t leaves;		/* bit i: leaf i is in the space */
  uint64_t next_badge;		/* for receive rights with no payload */
  uint32_t hint;		/* where the search for a free name starts */
  uint32_t fixed;		/* names below this are JN_FIXED */
};

/* Results: Mach's (mach/kern_return.h), so the wrappers pass them on.  */
#define JN_OK			0
#define JN_INVALID_VALUE	18	/* KERN_INVALID_VALUE */
#define JN_INVALID_NAME		15	/* KERN_INVALID_NAME */
#define JN_INVALID_RIGHT	17	/* KERN_INVALID_RIGHT */
#define JN_NO_SPACE		3	/* KERN_NO_SPACE */
#define JN_NAME_EXISTS		13	/* KERN_NAME_EXISTS */
#define JN_UREFS_OVERFLOW	19	/* KERN_UREFS_OVERFLOW */
#define JN_RESOURCE_SHORTAGE	6	/* KERN_RESOURCE_SHORTAGE */
#define JN_FAILURE		5	/* KERN_FAILURE */

/* Rights and dispositions: Mach's numbers.  */
#define JN_RIGHT_SEND		0
#define JN_RIGHT_RECEIVE	1
#define JN_RIGHT_SEND_ONCE	2
#define JN_RIGHT_PORT_SET	3
#define JN_RIGHT_DEAD_NAME	4
#define JN_MOVE_SEND		17
#define JN_MOVE_SEND_ONCE	18
#define JN_COPY_SEND		19
#define JN_MAKE_SEND		20
#define JN_MAKE_SEND_ONCE	21
/* Type bits (MACH_PORT_TYPE_*).  */
#define JN_TYPE_SEND		(1u << 16)
#define JN_TYPE_RECEIVE		(1u << 17)
#define JN_TYPE_PORT_SET	(1u << 19)
#define JN_TYPE_DEAD_NAME	(1u << 20)

#define JN_MAX_REFS 0xffffu

/* T, its names below FIXED given at start-up; leaf 0 is in every space.  */
extern void __jn_init (struct jn_table *t, uint32_t fixed);

/* mach_port_allocate: a new receive right, port set or dead name.  */
extern int __jn_allocate (struct jn_table *t, int right, uint32_t *name);
/* mach_port_deallocate: one user reference of a send right or dead
   name.  */
extern int __jn_deallocate (struct jn_table *t, uint32_t name);
/* mach_port_mod_refs.  */
extern int __jn_mod_refs (struct jn_table *t, uint32_t name, int right,
			  int delta);
/* mach_port_destroy: every right the name has.  */
extern int __jn_destroy (struct jn_table *t, uint32_t name);
/* mach_port_get_refs, mach_port_type.  */
extern int __jn_get_refs (const struct jn_table *t, uint32_t name, int right,
			  uint32_t *refs);
extern int __jn_type (const struct jn_table *t, uint32_t name,
		      uint32_t *type);
/* mach_port_insert_right: MAKE_SEND on a receive right's own name, or
   COPY_SEND / MOVE_SEND of the send right POLY to NAME (POLY's own name,
   or a new one).  */
extern int __jn_insert_right (struct jn_table *t, uint32_t name,
			      uint32_t poly, int disposition);
/* mach_port_move_member (SET 0: out of its set).  */
extern int __jn_move_member (struct jn_table *t, uint32_t member,
			     uint32_t set);
/* mach_port_set_protected_payload (0 clears it).  */
extern int __jn_set_payload (struct jn_table *t, uint32_t name,
			     uint64_t payload);

/* Messages.  A right to send: the slot to name in the message, and
   whether it is a slot made for this message alone (TEMP: delete it after
   the call).  MAKE_SEND on a receive right; COPY_SEND or MOVE_SEND of a
   send right.  */
extern int __jn_send_prepare (struct jn_table *t, uint32_t name,
			      int disposition, uint32_t *slot, int *temp);
/* After the call: SENT if the kernel took the message.  A MOVE_SEND then
   gives up one reference; a TEMP slot is deleted either way.  */
extern void __jn_send_done (struct jn_table *t, uint32_t name,
			    int disposition, uint32_t slot, int temp,
			    int sent);
/* A free name kept for a capability to arrive in.  */
extern int __jn_landing (struct jn_table *t, uint32_t *slot);
/* The kernel put a capability in the landing slot SLOT: a send right,
   one reference.  */
extern void __jn_arrived (struct jn_table *t, uint32_t slot);
/* The slot of the endpoint a receive right or port set receives on, or
   0 (a receive right not yet made).  */
extern uint32_t __jn_recv_slot (const struct jn_table *t, uint32_t name);
/* A receive right's badge (0 until its first send right).  */
extern uint64_t __jn_badge (const struct jn_table *t, uint32_t name);

#endif
