/* The task's address reservations on Joist (Phase 5 step 7, the design's
   item 6).
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

#include "joist-vm.h"

#include <string.h>

static uint64_t
end_of (const struct jv_res *r)
{
  return r->page + r->pages;
}

/* [PAGE, PAGE + PAGES) is a run in the user half past page 0.  */
static int
in_user (uint64_t page, uint64_t pages)
{
  return page >= 1 && page < JV_USER_PAGES && pages <= JV_USER_PAGES - page;
}

/* The first reservation that ends past PAGE (T->n if none).  */
static uint32_t
first_after (const struct jv_table *t, uint64_t page)
{
  uint32_t i = 0;
  while (i < t->n && end_of (&t->r[i]) <= page)
    i++;
  return i;
}

/* The reservation PAGE is strictly inside (past its first page), or
   T->n.  */
static uint32_t
inside (const struct jv_table *t, uint64_t page)
{
  uint32_t i = first_after (t, page);
  return i < t->n && t->r[i].page < page ? i : t->n;
}

/* Room at I for one more, which is a copy of the one there.  */
static void
open_at (struct jv_table *t, uint32_t i)
{
  memmove (&t->r[i + 1], &t->r[i], (t->n - i) * sizeof t->r[0]);
  t->n++;
}

/* The reservation PAGE is strictly inside cut in two at PAGE.  */
static void
split_at (struct jv_table *t, uint64_t page)
{
  uint32_t i = inside (t, page);
  if (i == t->n)
    return;
  uint64_t end = end_of (&t->r[i]);
  open_at (t, i);
  t->r[i].pages = page - t->r[i].page;
  t->r[i + 1].page = page;
  t->r[i + 1].pages = end - page;
}

void
__jv_init (struct jv_table *t)
{
  t->n = 0;
}

/* PAGE rounded up to a multiple of ALIGN, or JV_HI if that is past it.  */
static uint64_t
align_up (uint64_t page, uint64_t align)
{
  if (page > JV_HI - (align - 1))
    return JV_HI;
  return (page + align - 1) & ~(align - 1);
}

int
__jv_find (const struct jv_table *t, uint64_t pages, uint64_t align,
	   uint64_t *page)
{
  if (pages == 0 || align == 0 || (align & (align - 1)) != 0)
    return JV_INVALID_ARGUMENT;
  if (pages > JV_HI - JV_LO)
    return JV_NO_SPACE;
  uint64_t at = align_up (JV_LO, align);
  for (uint32_t i = first_after (t, at); i < t->n; i++)
    {
      if (at > JV_HI - pages)
	return JV_NO_SPACE;
      if (t->r[i].page >= at + pages)
	break;
      if (end_of (&t->r[i]) > at)
	at = align_up (end_of (&t->r[i]), align);
    }
  if (at > JV_HI - pages)
    return JV_NO_SPACE;
  *page = at;
  return JV_OK;
}

int
__jv_can_reserve (const struct jv_table *t, uint64_t page, uint64_t pages)
{
  if (pages == 0)
    return JV_INVALID_ARGUMENT;
  if (!in_user (page, pages))
    return JV_INVALID_ADDRESS;
  uint32_t i = first_after (t, page);
  if (i < t->n && t->r[i].page < page + pages)
    return JV_NO_SPACE;
  if (t->n == JV_MAX)
    return JV_RESOURCE_SHORTAGE;
  return JV_OK;
}

void
__jv_reserve (struct jv_table *t, uint64_t page, uint64_t pages,
	      uint32_t prot)
{
  uint32_t i = first_after (t, page);
  open_at (t, i);
  t->r[i].page = page;
  t->r[i].pages = pages;
  t->r[i].prot = prot;
}

int
__jv_can_release (const struct jv_table *t, uint64_t page, uint64_t pages)
{
  if (pages == 0)
    return JV_OK;
  if (!in_user (page, pages))
    return JV_INVALID_ADDRESS;
  uint32_t i = inside (t, page);
  if (i < t->n && end_of (&t->r[i]) > page + pages && t->n == JV_MAX)
    return JV_RESOURCE_SHORTAGE;	/* cut in two */
  return JV_OK;
}

void
__jv_release (struct jv_table *t, uint64_t page, uint64_t pages)
{
  if (pages == 0)
    return;
  uint64_t end = page + pages;
  uint32_t i = first_after (t, page);
  if (i < t->n && t->r[i].page < page)
    {
      uint64_t e = end_of (&t->r[i]);
      if (e > end)
	{
	  /* Inside one reservation: cut it in two (the one more
	     __jv_can_release checked room for).  */
	  open_at (t, i);
	  t->r[i].pages = page - t->r[i].page;
	  t->r[i + 1].page = end;
	  t->r[i + 1].pages = e - end;
	  return;
	}
      t->r[i].pages = page - t->r[i].page;	/* its tail goes */
      i++;
    }
  uint32_t j = i;
  while (j < t->n && end_of (&t->r[j]) <= end)
    j++;			/* [i, j): wholly inside the range */
  if (j < t->n && t->r[j].page < end)
    {
      t->r[j].pages = end_of (&t->r[j]) - end;	/* its head goes */
      t->r[j].page = end;
    }
  memmove (&t->r[i], &t->r[j], (t->n - j) * sizeof t->r[0]);
  t->n -= j - i;
}

int
__jv_can_protect (const struct jv_table *t, uint64_t page, uint64_t pages)
{
  if (pages == 0)
    return JV_OK;
  if (!in_user (page, pages))
    return JV_INVALID_ADDRESS;
  uint64_t at = page, end = page + pages;
  for (uint32_t i = first_after (t, page); at < end; i++)
    {
      if (i == t->n || t->r[i].page > at)
	return JV_INVALID_ADDRESS;	/* a hole */
      at = end_of (&t->r[i]);
    }
  uint32_t more = (inside (t, page) < t->n) + (inside (t, end) < t->n);
  if (more > JV_MAX - t->n)
    return JV_RESOURCE_SHORTAGE;
  return JV_OK;
}

void
__jv_protect (struct jv_table *t, uint64_t page, uint64_t pages,
	      uint32_t prot)
{
  if (pages == 0)
    return;
  uint64_t end = page + pages;
  split_at (t, page);
  split_at (t, end);
  for (uint32_t i = first_after (t, page); i < t->n && t->r[i].page < end;
       i++)
    t->r[i].prot = prot;
}

int
__jv_region (const struct jv_table *t, uint64_t page, struct jv_res *r)
{
  uint32_t i = first_after (t, page);
  if (i == t->n)
    return JV_NO_SPACE;
  *r = t->r[i];
  return JV_OK;
}

int
__jv_check (const struct jv_table *t)
{
  if (t->n > JV_MAX)
    return 1;
  for (uint32_t i = 0; i < t->n; i++)
    {
      const struct jv_res *r = &t->r[i];
      if (r->pages == 0)
	return 2;
      if (!in_user (r->page, r->pages))
	return 3;
      if (i > 0 && r->page < end_of (&t->r[i - 1]))
	return 4;		/* out of order, or overlapping */
    }
  return 0;
}

/* The vm_* calls.  */

/* The pages of [ADDR, ADDR + SIZE): 0 if past the address space.  */
static int
pages_of (uint64_t addr, uint64_t size, uint64_t *page, uint64_t *pages)
{
  uint64_t mask = (1ull << JV_PAGE_SHIFT) - 1;
  if (size > UINT64_MAX - (addr & mask) - mask)
    return 0;
  *page = addr >> JV_PAGE_SHIFT;
  *pages = ((addr & mask) + size + mask) >> JV_PAGE_SHIFT;
  return 1;
}

/* Mach's protection as mm's permissions: 0 if mm can't map it.  */
static int
perms_of (uint32_t prot, uint32_t *perms)
{
  if ((prot & JV_PROT_WRITE) && (prot & JV_PROT_EXECUTE))
    return 0;
  if ((prot & (JV_PROT_READ | JV_PROT_WRITE | JV_PROT_EXECUTE)) == 0)
    *perms = JV_MM_NONE;
  else
    *perms = (prot & JV_PROT_WRITE ? JV_MM_W : 0)
	     | (prot & JV_PROT_EXECUTE ? JV_MM_X : 0);
  return 1;
}

static int
mm_result (int e)
{
  switch (e)
    {
    case 0:
      return JV_OK;
    case 1:
      return JV_INVALID_ADDRESS;
    case 2:
      return JV_RESOURCE_SHORTAGE;
    case 3:
      return JV_NO_SPACE;
    default:
      return JV_FAILURE;
    }
}

int
__jv_allocate (struct jv_table *t, uint64_t *addr, uint64_t size,
	       uint64_t mask, int anywhere, uint32_t prot)
{
  uint64_t page, pages;
  uint32_t perms;
  if (size == 0 || !pages_of (anywhere ? 0 : *addr, size, &page, &pages))
    return JV_INVALID_ARGUMENT;
  if (!perms_of (prot, &perms))
    return JV_PROTECTION_FAILURE;
  if (anywhere)
    {
      uint64_t align = (mask >> JV_PAGE_SHIFT) + 1;
      if ((mask & (mask + 1)) != 0)
	return JV_INVALID_ARGUMENT;	/* not a power of 2, less one */
      int e = __jv_find (t, pages, align, &page);
      if (e != JV_OK)
	return e;
    }
  int e = __jv_can_reserve (t, page, pages);
  if (e != JV_OK)
    return e;
  e = mm_result (__joist_vm_mm (JV_MM_MAP_ANON, page, pages, perms));
  if (e != JV_OK)
    return e;
  __jv_reserve (t, page, pages, prot);
  *addr = page << JV_PAGE_SHIFT;
  return JV_OK;
}

int
__jv_deallocate (struct jv_table *t, uint64_t addr, uint64_t size)
{
  uint64_t page, pages;
  if (size == 0)
    return JV_OK;
  if (!pages_of (addr, size, &page, &pages))
    return JV_INVALID_ADDRESS;
  int e = __jv_can_release (t, page, pages);
  if (e != JV_OK)
    return e;
  e = mm_result (__joist_vm_mm (JV_MM_UNMAP, page, pages, 0));
  if (e != JV_OK)
    return e;
  __jv_release (t, page, pages);
  return JV_OK;
}

int
__jv_vm_protect (struct jv_table *t, uint64_t addr, uint64_t size,
		 uint32_t prot)
{
  uint64_t page, pages;
  uint32_t perms;
  if (size == 0)
    return JV_OK;
  if (!pages_of (addr, size, &page, &pages))
    return JV_INVALID_ADDRESS;
  if (!perms_of (prot, &perms))
    return JV_PROTECTION_FAILURE;
  int e = __jv_can_protect (t, page, pages);
  if (e != JV_OK)
    return e;
  e = mm_result (__joist_vm_mm (JV_MM_PROTECT, page, pages, perms));
  if (e != JV_OK)
    return e;
  __jv_protect (t, page, pages, prot);
  return JV_OK;
}

int
__jv_vm_region (const struct jv_table *t, uint64_t *addr, uint64_t *size,
		uint32_t *prot)
{
  struct jv_res r;
  int e = __jv_region (t, *addr >> JV_PAGE_SHIFT, &r);
  if (e != JV_OK)
    return e;
  *addr = r.page << JV_PAGE_SHIFT;
  *size = r.pages << JV_PAGE_SHIFT;
  *prot = r.prot;
  return JV_OK;
}
