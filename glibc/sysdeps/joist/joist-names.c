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

#include "joist-names.h"

#include <stddef.h>

/* Badges for receive rights without a payload: above any address, so
   never one a payload (a pointer) could be.  */
#define FIRST_BADGE (1ull << 63)

void
__jn_init (struct jn_table *t, uint32_t fixed)
{
  if (fixed == 0)
    fixed = 1;			/* name 0 is MACH_PORT_NULL: never a name */
  for (uint32_t i = 0; i < JN_NAMES; i++)
    {
      struct jn_entry *e = &t->e[i];
      e->kind = i < fixed ? JN_FIXED : JN_FREE;
      e->minted = 0;
      e->refs = 0;
      e->set = e->ep = 0;
      e->badge = 0;
    }
  t->leaves = 1;
  t->next_badge = FIRST_BADGE;
  t->hint = fixed;
  t->fixed = fixed;
}

static int
kern (int e)
{
  return e == 0 ? JN_OK
	 : e == JN_KERN_NOMEM ? JN_RESOURCE_SHORTAGE : JN_FAILURE;
}

/* The entry of NAME, if NAME is one a caller may use (not a free name, or
   one the table keeps for itself).  */
static struct jn_entry *
lookup (struct jn_table *t, uint32_t name)
{
  if (name == 0 || name >= JN_NAMES)
    return NULL;
  struct jn_entry *e = &t->e[name];
  if (e->kind == JN_FREE || e->kind == JN_HIDDEN || e->kind == JN_LANDING)
    return NULL;
  return e;
}

static const struct jn_entry *
lookup_c (const struct jn_table *t, uint32_t name)
{
  return lookup ((struct jn_table *) t, name);
}

/* A free name, made KIND: in a leaf the space has, or in the first leaf
   it hasn't, grown for it (its index in *GREW, else -1).  */
static int
take_name (struct jn_table *t, int kind, uint32_t *name, int *grew)
{
  *grew = -1;
  for (uint32_t n = 0; n < JN_NAMES; n++)
    {
      uint32_t i = t->hint + n;
      if (i >= JN_NAMES)
	i -= JN_NAMES - t->fixed;
      if (i < t->fixed)
	continue;
      if ((t->leaves >> (i / JN_LEAF) & 1) && t->e[i].kind == JN_FREE)
	{
	  t->e[i].kind = (uint8_t) kind;
	  t->hint = i + 1 < JN_NAMES ? i + 1 : t->fixed;
	  *name = i;
	  return JN_OK;
	}
    }
  for (uint32_t leaf = 1; leaf < JN_LEAVES; leaf++)
    if (!(t->leaves >> leaf & 1))
      {
	int e = kern (__joist_names_grow (leaf));
	if (e != JN_OK)
	  return e == JN_RESOURCE_SHORTAGE ? JN_NO_SPACE : e;
	t->leaves |= 1ull << leaf;
	uint32_t i = leaf * JN_LEAF;
	t->e[i].kind = (uint8_t) kind;
	t->hint = i + 1;
	*name = i;
	*grew = (int) leaf;
	return JN_OK;
      }
  return JN_NO_SPACE;
}

static void
free_name (struct jn_table *t, uint32_t name)
{
  struct jn_entry *e = &t->e[name];
  e->kind = JN_FREE;
  e->minted = 0;
  e->refs = 0;
  e->set = e->ep = 0;
  e->badge = 0;
}

/* A name taken for a call that then failed, given back with the leaf
   grown for it (GREW, or -1).  */
static void
untake (struct jn_table *t, uint32_t name, int grew)
{
  free_name (t, name);
  if (grew >= 0)
    {
      (void) __joist_names_shrink ((uint32_t) grew);
      t->leaves &= ~(1ull << grew);
    }
}

/* What a call that fails part way puts back: the table's own state
   besides the names it took (which untake gives back).  */
struct undo
{
  uint32_t hint;
  uint64_t next_badge;
  struct jn_entry e;		/* the receive right fix_badge changed */
  uint32_t hidden;		/* the endpoint it made, or 0 */
  int grew;			/* the leaf grown for it, or -1 */
};

static void
undo_begin (struct jn_table *t, uint32_t name, struct undo *u)
{
  u->hint = t->hint;
  u->next_badge = t->next_badge;
  u->e = t->e[name];
  u->hidden = 0;
  u->grew = -1;
}

static void
undo_all (struct jn_table *t, uint32_t name, const struct undo *u)
{
  if (u->hidden != 0)
    {
      (void) __joist_names_delete (u->hidden);
      untake (t, u->hidden, u->grew);
    }
  t->e[name] = u->e;
  t->hint = u->hint;
  t->next_badge = u->next_badge;
}

/* A receive right's badge fixed, and the endpoint it is on made (its own,
   if it is in no set).  */
static int
fix_badge (struct jn_table *t, uint32_t name, struct undo *u)
{
  struct jn_entry *e = &t->e[name];
  if (e->minted)
    return JN_OK;
  if (e->set == 0)
    {
      uint32_t h;
      int g;
      int r = take_name (t, JN_HIDDEN, &h, &g);
      if (r != JN_OK)
	return r;
      r = kern (__joist_names_endpoint (h));
      if (r != JN_OK)
	{
	  untake (t, h, g);
	  return r;
	}
      e->ep = h;
      u->hidden = h;
      u->grew = g;
    }
  if (e->badge == 0)
    e->badge = t->next_badge++;
  e->minted = 1;
  return JN_OK;
}

/* A send capability with a receive right's badge, into DST.  */
static int
mint_into (struct jn_table *t, uint32_t name, uint32_t dst)
{
  struct jn_entry *e = &t->e[name];
  uint32_t src = e->set != 0 ? e->set : e->ep;
  if (src == 0)
    return JN_FAILURE;		/* its set was destroyed */
  return kern (__joist_names_mint (src, dst, e->badge));
}

int
__jn_allocate (struct jn_table *t, int right, uint32_t *name)
{
  uint32_t n, hint = t->hint;
  int r, g;
  switch (right)
    {
    case JN_RIGHT_RECEIVE:
      return take_name (t, JN_RECEIVE, name, &g);
    case JN_RIGHT_PORT_SET:
      r = take_name (t, JN_PORTSET, &n, &g);
      if (r != JN_OK)
	return r;
      r = kern (__joist_names_endpoint (n));
      if (r != JN_OK)
	{
	  untake (t, n, g);
	  t->hint = hint;
	  return r;
	}
      *name = n;
      return JN_OK;
    case JN_RIGHT_DEAD_NAME:
      r = take_name (t, JN_DEAD, &n, &g);
      if (r != JN_OK)
	return r;
      t->e[n].refs = 1;
      *name = n;
      return JN_OK;
    default:
      return JN_INVALID_VALUE;
    }
}

int
__jn_reply_name (struct jn_table *t, uint32_t *name)
{
  int g;
  return take_name (t, JN_REPLY, name, &g);
}

/* A send right's (or receive right's own send right's) references to
   NEW, the capability deleted at 0.  */
static int
set_send_refs (struct jn_table *t, uint32_t name, uint32_t new)
{
  struct jn_entry *e = &t->e[name];
  if (new == 0)
    {
      int r = kern (__joist_names_delete (name));
      if (r != JN_OK)
	return r;
      if (e->kind == JN_SEND)
	free_name (t, name);
      else
	e->refs = 0;
      return JN_OK;
    }
  e->refs = (uint16_t) new;
  return JN_OK;
}

static int
has_send (const struct jn_entry *e)
{
  return e->kind == JN_SEND || (e->kind == JN_RECEIVE && e->refs > 0);
}

int
__jn_deallocate (struct jn_table *t, uint32_t name)
{
  struct jn_entry *e = lookup (t, name);
  if (e == NULL)
    return JN_INVALID_NAME;
  if (e->kind == JN_FIXED)
    return JN_OK;
  if (has_send (e))
    return set_send_refs (t, name, e->refs - 1u);
  if (e->kind == JN_DEAD)
    {
      if (--e->refs == 0)
	free_name (t, name);
      return JN_OK;
    }
  return JN_INVALID_RIGHT;
}

/* A receive right's end: its own endpoint, its send capability; the
   name a dead name if it had send references, else free.  */
static int
end_receive (struct jn_table *t, uint32_t name)
{
  struct jn_entry *e = &t->e[name];
  if (e->refs > 0)
    {
      int r = kern (__joist_names_delete (name));
      if (r != JN_OK)
	return r;
    }
  if (e->ep != 0)
    {
      (void) __joist_names_delete (e->ep);	/* its senders' calls fail */
      free_name (t, e->ep);
    }
  uint16_t refs = e->refs;
  free_name (t, name);
  if (refs > 0)
    {
      e->kind = JN_DEAD;
      e->refs = refs;
    }
  return JN_OK;
}

static int
end_portset (struct jn_table *t, uint32_t name)
{
  int r = kern (__joist_names_delete (name));
  if (r != JN_OK)
    return r;
  for (uint32_t i = t->fixed; i < JN_NAMES; i++)
    if (t->e[i].kind == JN_RECEIVE && t->e[i].set == name)
      t->e[i].set = 0;		/* a minted one can make no more rights */
  free_name (t, name);
  return JN_OK;
}

int
__jn_mod_refs (struct jn_table *t, uint32_t name, int right, int delta)
{
  struct jn_entry *e = lookup (t, name);
  if (e == NULL)
    return JN_INVALID_NAME;
  if (e->kind == JN_FIXED)
    return right == JN_RIGHT_SEND || right == JN_RIGHT_SEND_ONCE
	   ? JN_OK : JN_INVALID_RIGHT;
  switch (right)
    {
    case JN_RIGHT_SEND:
    case JN_RIGHT_SEND_ONCE:
    case JN_RIGHT_DEAD_NAME:
      {
	int dead = right == JN_RIGHT_DEAD_NAME;
	if (dead ? e->kind != JN_DEAD : !has_send (e))
	  return delta == 0 && !dead && e->kind == JN_RECEIVE
		 ? JN_OK : JN_INVALID_RIGHT;
	int64_t n = (int64_t) e->refs + delta;
	if (n < 0)
	  return JN_INVALID_VALUE;
	if (n > JN_MAX_REFS)
	  return JN_UREFS_OVERFLOW;
	if (dead)
	  {
	    if (n == 0)
	      free_name (t, name);
	    else
	      e->refs = (uint16_t) n;
	    return JN_OK;
	  }
	return set_send_refs (t, name, (uint32_t) n);
      }
    case JN_RIGHT_RECEIVE:
      if (e->kind != JN_RECEIVE && e->kind != JN_REPLY)
	return JN_INVALID_RIGHT;
      if (delta < -1 || delta > 0)
	return JN_INVALID_VALUE;
      if (delta == 0)
	return JN_OK;
      if (e->kind == JN_REPLY)
	{
	  free_name (t, name);
	  return JN_OK;
	}
      return end_receive (t, name);
    case JN_RIGHT_PORT_SET:
      if (e->kind != JN_PORTSET)
	return JN_INVALID_RIGHT;
      if (delta < -1 || delta > 0)
	return JN_INVALID_VALUE;
      return delta == 0 ? JN_OK : end_portset (t, name);
    default:
      return JN_INVALID_VALUE;
    }
}

int
__jn_destroy (struct jn_table *t, uint32_t name)
{
  struct jn_entry *e = lookup (t, name);
  if (e == NULL)
    return JN_INVALID_NAME;
  switch (e->kind)
    {
    case JN_FIXED:
      return JN_OK;
    case JN_SEND:
      return set_send_refs (t, name, 0);
    case JN_RECEIVE:
      {
	int r = end_receive (t, name);
	if (r == JN_OK && t->e[name].kind == JN_DEAD)
	  free_name (t, name);
	return r;
      }
    case JN_PORTSET:
      return end_portset (t, name);
    default:			/* JN_DEAD, JN_REPLY */
      free_name (t, name);
      return JN_OK;
    }
}

int
__jn_get_refs (const struct jn_table *t, uint32_t name, int right,
	       uint32_t *refs)
{
  const struct jn_entry *e = lookup_c (t, name);
  if (e == NULL)
    return JN_INVALID_NAME;
  switch (right)
    {
    case JN_RIGHT_SEND:
      *refs = e->kind == JN_FIXED ? 1u : has_send (e) ? e->refs : 0u;
      return JN_OK;
    case JN_RIGHT_SEND_ONCE:
      *refs = 0;
      return JN_OK;
    case JN_RIGHT_RECEIVE:
      *refs = e->kind == JN_RECEIVE || e->kind == JN_REPLY;
      return JN_OK;
    case JN_RIGHT_PORT_SET:
      *refs = e->kind == JN_PORTSET;
      return JN_OK;
    case JN_RIGHT_DEAD_NAME:
      *refs = e->kind == JN_DEAD ? e->refs : 0u;
      return JN_OK;
    default:
      return JN_INVALID_VALUE;
    }
}

int
__jn_type (const struct jn_table *t, uint32_t name, uint32_t *type)
{
  const struct jn_entry *e = lookup_c (t, name);
  if (e == NULL)
    return JN_INVALID_NAME;
  uint32_t ty = 0;
  if (e->kind == JN_FIXED || has_send (e))
    ty |= JN_TYPE_SEND;
  if (e->kind == JN_RECEIVE || e->kind == JN_REPLY)
    ty |= JN_TYPE_RECEIVE;
  if (e->kind == JN_PORTSET)
    ty |= JN_TYPE_PORT_SET;
  if (e->kind == JN_DEAD)
    ty |= JN_TYPE_DEAD_NAME;
  *type = ty;
  return JN_OK;
}

/* One more send reference on a receive right's own name, its capability
   minted at the first.  */
static int
make_send (struct jn_table *t, uint32_t name)
{
  struct jn_entry *e = &t->e[name];
  if (e->refs == JN_MAX_REFS)
    return JN_UREFS_OVERFLOW;
  struct undo u;
  undo_begin (t, name, &u);
  int r = fix_badge (t, name, &u);
  if (r == JN_OK && e->refs == 0)
    r = mint_into (t, name, name);
  if (r != JN_OK)
    {
      undo_all (t, name, &u);
      return r;
    }
  e->refs++;
  return JN_OK;
}

/* NAME, if it is free, made usable: its leaf grown if the space hasn't
   it.  */
static int
claim (struct jn_table *t, uint32_t name, int kind, int *grew)
{
  *grew = -1;
  if (name < t->fixed || name >= JN_NAMES)
    return JN_INVALID_VALUE;
  if (t->e[name].kind != JN_FREE)
    return JN_NAME_EXISTS;
  uint32_t leaf = name / JN_LEAF;
  if (!(t->leaves >> leaf & 1))
    {
      int r = kern (__joist_names_grow (leaf));
      if (r != JN_OK)
	return r == JN_RESOURCE_SHORTAGE ? JN_NO_SPACE : r;
      t->leaves |= 1ull << leaf;
      *grew = (int) leaf;
    }
  t->e[name].kind = (uint8_t) kind;
  return JN_OK;
}

int
__jn_insert_right (struct jn_table *t, uint32_t name, uint32_t poly,
		   int disposition)
{
  if (disposition == JN_MAKE_SEND || disposition == JN_MAKE_SEND_ONCE)
    {
      /* Mach's way of giving a receive right a send right of its own.  */
      struct jn_entry *e = lookup (t, poly);
      if (e == NULL)
	return JN_INVALID_NAME;
      if (e->kind != JN_RECEIVE)
	return JN_INVALID_RIGHT;
      if (name != poly)
	return JN_INVALID_VALUE;	/* one name per port, here as in Mach */
      return make_send (t, name);
    }
  if (disposition != JN_COPY_SEND && disposition != JN_MOVE_SEND
      && disposition != JN_MOVE_SEND_ONCE)
    return JN_INVALID_VALUE;
  struct jn_entry *p = lookup (t, poly);
  if (p == NULL)
    return JN_INVALID_NAME;
  if (p->kind != JN_FIXED && !has_send (p))
    return JN_INVALID_RIGHT;
  int move = disposition != JN_COPY_SEND;
  if (name == poly)
    {
      /* A copy into its own name is one more reference; a move, none.  */
      if (move || p->kind == JN_FIXED)
	return JN_OK;
      if (p->refs == JN_MAX_REFS)
	return JN_UREFS_OVERFLOW;
      p->refs++;
      return JN_OK;
    }
  int g;
  int r = claim (t, name, JN_SEND, &g);
  if (r != JN_OK)
    return r;
  r = kern (__joist_names_derive (poly, name));
  if (r != JN_OK)
    {
      untake (t, name, g);
      return r;
    }
  t->e[name].refs = 1;
  return move ? __jn_deallocate (t, poly) : JN_OK;
}

int
__jn_move_member (struct jn_table *t, uint32_t member, uint32_t set)
{
  struct jn_entry *e = lookup (t, member);
  if (e == NULL)
    return JN_INVALID_NAME;
  if (e->kind != JN_RECEIVE)
    return JN_INVALID_RIGHT;
  if (set != 0)
    {
      struct jn_entry *s = lookup (t, set);
      if (s == NULL)
	return JN_INVALID_NAME;
      if (s->kind != JN_PORTSET)
	return JN_INVALID_RIGHT;
    }
  if (e->set == set)
    return JN_OK;
  if (e->minted)
    return JN_INVALID_RIGHT;	/* 0005: nothing moves once it has a badge */
  e->set = set;
  return JN_OK;
}

int
__jn_set_payload (struct jn_table *t, uint32_t name, uint64_t payload)
{
  struct jn_entry *e = lookup (t, name);
  if (e == NULL)
    return JN_INVALID_NAME;
  if (e->kind != JN_RECEIVE)
    return JN_INVALID_RIGHT;
  if (e->badge == payload)
    return JN_OK;
  if (e->minted || payload >= FIRST_BADGE)
    return JN_INVALID_RIGHT;
  e->badge = payload;
  return JN_OK;
}

int
__jn_send_prepare (struct jn_table *t, uint32_t name, int disposition,
		   uint32_t *slot, int *temp)
{
  struct jn_entry *e = lookup (t, name);
  if (e == NULL)
    return JN_INVALID_NAME;
  switch (disposition)
    {
    case JN_MAKE_SEND:
    case JN_MAKE_SEND_ONCE:
      {
	if (e->kind != JN_RECEIVE)
	  return JN_INVALID_RIGHT;
	struct undo u;
	undo_begin (t, name, &u);
	int r = fix_badge (t, name, &u);
	uint32_t s = 0;
	int g = -1;
	if (r == JN_OK)
	  r = take_name (t, JN_LANDING, &s, &g);
	if (r == JN_OK)
	  {
	    r = mint_into (t, name, s);
	    if (r != JN_OK)
	      untake (t, s, g);
	  }
	if (r != JN_OK)
	  {
	    undo_all (t, name, &u);
	    return r;
	  }
	*slot = s;
	*temp = 1;
	return JN_OK;
      }
    case JN_COPY_SEND:
    case JN_MOVE_SEND:
    case JN_MOVE_SEND_ONCE:
      if (e->kind != JN_FIXED && !has_send (e))
	return JN_INVALID_RIGHT;
      *slot = name;
      *temp = 0;
      return JN_OK;
    default:
      return JN_INVALID_VALUE;
    }
}

void
__jn_send_done (struct jn_table *t, uint32_t name, int disposition,
		uint32_t slot, int temp, int sent)
{
  if (temp)
    {
      (void) __joist_names_delete (slot);
      free_name (t, slot);
      return;
    }
  if (sent && (disposition == JN_MOVE_SEND
	       || disposition == JN_MOVE_SEND_ONCE))
    (void) __jn_deallocate (t, name);
}

int
__jn_landing (struct jn_table *t, uint32_t *slot)
{
  int g;
  return take_name (t, JN_LANDING, slot, &g);
}

void
__jn_arrived (struct jn_table *t, uint32_t slot)
{
  if (slot < JN_NAMES && t->e[slot].kind == JN_LANDING)
    {
      t->e[slot].kind = JN_SEND;
      t->e[slot].refs = 1;
    }
}

void
__jn_unland (struct jn_table *t, uint32_t slot)
{
  if (slot < JN_NAMES && t->e[slot].kind == JN_LANDING)
    free_name (t, slot);
}

uint32_t
__jn_recv_slot (const struct jn_table *t, uint32_t name)
{
  const struct jn_entry *e = lookup_c (t, name);
  if (e == NULL)
    return 0;
  if (e->kind == JN_PORTSET)
    return name;
  if (e->kind == JN_RECEIVE)
    return e->set != 0 ? e->set : e->ep;
  return 0;
}

uint64_t
__jn_badge (const struct jn_table *t, uint32_t name)
{
  const struct jn_entry *e = lookup_c (t, name);
  return e != NULL && e->kind == JN_RECEIVE && e->minted ? e->badge : 0;
}
