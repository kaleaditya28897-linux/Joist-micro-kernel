/* The pages of libc's write-window region on Joist (Phase 5 step 9, the
   owner's choice of 2026-11-16).
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

#include "joist-winmem.h"

/* The loops move entries by hand, as joist-vm.c's do: no library call in
   the bookkeeping.  Every call is a scan of at most JW_RUNS runs.  */

static uint32_t
end_of (const struct jw_run *r)
{
  return r->page + r->pages;
}

/* Extent K's pages (K below JW_EXTENTS).  */
static uint32_t
extent_size (uint32_t k)
{
  return k == 0 ? JW_FIRST : JW_FIRST << (k - 1u);
}

void
__jw_init (struct jw_table *t)
{
  t->mapped = 0;
  t->extents = 0;
  t->slices = 0;
  t->n = 0;
}

uint32_t
__jw_extent_pages (const struct jw_table *t)
{
  return t->extents < JW_EXTENTS ? extent_size (t->extents) : 0;
}

/* The first free run that holds PAGES, or T->n.  */
static uint32_t
first_fit (const struct jw_table *t, uint32_t pages)
{
  uint32_t i = 0;
  while (i < t->n && t->r[i].pages < pages)
    i++;
  return i;
}

int
__jw_plan (const struct jw_table *t, uint32_t pages, uint32_t *grow)
{
  if (pages == 0)
    return JW_INVALID;
  if (t->slices >= JW_RUNS - 1u)
    return JW_FULL;
  if (first_fit (t, pages) < t->n)
    {
      *grow = 0;
      return JW_OK;
    }
  /* Grown, the last run is the free tail of the mapped part with every
     extent added after it.  */
  uint32_t room = 0;
  if (t->n > 0 && end_of (&t->r[t->n - 1u]) == t->mapped)
    room = t->r[t->n - 1u].pages;
  for (uint32_t k = t->extents; k < JW_EXTENTS; k++)
    {
      room += extent_size (k);
      if (room >= pages)
	{
	  *grow = k + 1u - t->extents;
	  return JW_OK;
	}
    }
  return JW_NONE;
}

int
__jw_grow (struct jw_table *t, uint32_t pages)
{
  if (pages == 0 || pages != __jw_extent_pages (t))
    return JW_INVALID;
  if (t->n > 0 && end_of (&t->r[t->n - 1u]) == t->mapped)
    t->r[t->n - 1u].pages += pages;	/* the free tail runs on */
  else if (t->n == JW_RUNS)
    return JW_FULL;
  else
    {
      t->r[t->n].page = t->mapped;
      t->r[t->n].pages = pages;
      t->n++;
    }
  t->mapped += pages;
  t->extents++;
  return JW_OK;
}

int
__jw_take (struct jw_table *t, uint32_t pages, uint32_t *page)
{
  if (pages == 0)
    return JW_INVALID;
  if (t->slices >= JW_RUNS - 1u)
    return JW_FULL;
  uint32_t i = first_fit (t, pages);
  if (i == t->n)
    return JW_NONE;
  *page = t->r[i].page;
  if (t->r[i].pages == pages)
    {
      for (uint32_t k = i + 1u; k < t->n; k++)
	t->r[k - 1u] = t->r[k];
      t->n--;
    }
  else
    {
      t->r[i].page += pages;
      t->r[i].pages -= pages;
    }
  t->slices++;
  return JW_OK;
}

int
__jw_give (struct jw_table *t, uint32_t page, uint32_t pages)
{
  if (pages == 0 || t->slices == 0 || page >= t->mapped
      || pages > t->mapped - page)
    return JW_INVALID;
  uint32_t end = page + pages;
  /* I: the first run that ends past PAGE.  */
  uint32_t i = 0;
  while (i < t->n && end_of (&t->r[i]) <= page)
    i++;
  if (i < t->n && t->r[i].page < end)
    return JW_INVALID;		/* some of it is free: not a slice out */
  int before = i > 0 && end_of (&t->r[i - 1u]) == page;
  int after = i < t->n && t->r[i].page == end;
  if (before && after)
    {
      /* It joins the two runs about it.  */
      t->r[i - 1u].pages += pages + t->r[i].pages;
      for (uint32_t k = i + 1u; k < t->n; k++)
	t->r[k - 1u] = t->r[k];
      t->n--;
    }
  else if (before)
    t->r[i - 1u].pages += pages;
  else if (after)
    {
      t->r[i].page = page;
      t->r[i].pages += pages;
    }
  else
    {
      if (t->n == JW_RUNS)
	return JW_FULL;
      for (uint32_t k = t->n; k > i; k--)
	t->r[k] = t->r[k - 1u];
      t->r[i].page = page;
      t->r[i].pages = pages;
      t->n++;
    }
  t->slices--;
  return JW_OK;
}

int
__jw_check (const struct jw_table *t)
{
  if (t->n > JW_RUNS)
    return 1;
  if (t->slices > JW_RUNS - 1u)
    return 2;
  if (t->extents > JW_EXTENTS)
    return 3;
  uint32_t mapped = 0;
  for (uint32_t k = 0; k < t->extents; k++)
    mapped += extent_size (k);
  if (t->mapped != mapped)
    return 4;
  uint32_t nfree = 0;
  for (uint32_t i = 0; i < t->n; i++)
    {
      const struct jw_run *r = &t->r[i];
      if (r->pages == 0)
	return 5;
      if (r->page >= t->mapped || r->pages > t->mapped - r->page)
	return 6;
      if (i > 0 && r->page <= end_of (&t->r[i - 1u]))
	return 7;		/* out of order, overlapping or touching */
      nfree += r->pages;
    }
  /* Runs apart have a slice out between them; none out, all is free.  */
  if (t->n > t->slices + 1u)
    return 8;
  if (t->slices == 0 && nfree != t->mapped)
    return 9;
  if (t->slices > t->mapped - nfree)
    return 10;			/* a slice is a page at least */
  return 0;
}
