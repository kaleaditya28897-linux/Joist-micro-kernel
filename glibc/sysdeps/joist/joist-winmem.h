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

/* A write window must lie in one of the task's regions (decisions 0030,
   0032).  A task has one region for them, JW_PAGES long (32 page tables,
   64 MiB), grown from its start an extent of zeroed pages at a time
   (mm's MM_REQ_REGION_GROW, mapped writable by the task itself), each
   extent twice the one before past the first two: JW_FIRST pages, then
   JW_FIRST, 2 JW_FIRST, ... in JW_EXTENTS (256 pages: 1, 1, 2, 4, 8, 16,
   32 MiB, the region's 64 in 7, of the 8 extents a region may have).
   Pages stay mapped once grown.  A call's write window is a slice of the
   mapped part, whole pages, handed out first fit and given back after the
   call.

   This file holds the bookkeeping alone, as pure functions on a table, so
   that the host tests (model/test/test_winmem.c) run them as they are;
   joist-mig.c does the locking and asks mm and the kernel.  Pages
   throughout, counted from the region's first page.

   The table holds the free runs of the mapped part, sorted, none empty,
   none touching another (a run given back merges with its neighbours).
   Free runs are separated by pages handed out, so there are at most one
   more of them than slices out; slices out are kept below JW_RUNS, so a
   run given back, or an extent grown, always has room in the table.  */

#ifndef _JOIST_WINMEM_H
#define _JOIST_WINMEM_H 1

#include <stdint.h>

/* Free runs at most; slices out at most one fewer.  */
#ifndef JW_RUNS
# define JW_RUNS 32u
#endif
/* The first extent's pages, and how many extents there are.  */
#ifndef JW_FIRST
# define JW_FIRST 256u
#endif
#ifndef JW_EXTENTS
# define JW_EXTENTS 7u
#endif
/* The region: the extents' pages together, JW_FIRST << (JW_EXTENTS - 1).  */
#define JW_PAGES (JW_FIRST << (JW_EXTENTS - 1u))

/* Results.  */
#define JW_OK		0
#define JW_INVALID	1	/* no pages, past the mapped part, or not out */
#define JW_FULL		2	/* the table has no room (slices or runs) */
#define JW_NONE		3	/* no run that long, even grown to the end */

struct jw_run
{
  uint32_t page, pages;
};

struct jw_table
{
  uint32_t mapped;		/* pages grown and mapped, from the start */
  uint32_t extents;		/* extents grown */
  uint32_t slices;		/* slices out */
  uint32_t n;			/* free runs */
  struct jw_run r[JW_RUNS];
};

extern void __jw_init (struct jw_table *t);

/* The next extent's pages: 0 if the region has all of its extents.  */
extern uint32_t __jw_extent_pages (const struct jw_table *t);

/* What a slice of PAGES needs: JW_OK, *GROW the extents to grow first (0
   if a free run holds it now: the first fit, or the last run's room once
   grown, as __jw_take will take it); JW_INVALID (no pages), JW_FULL (as
   many slices out as there may be), or JW_NONE (the region grown to its
   end would hold no run that long).  Nothing changes.  */
extern int __jw_plan (const struct jw_table *t, uint32_t pages,
		      uint32_t *grow);

/* The next extent, of __jw_extent_pages's PAGES, grown and mapped at the
   end of the mapped part: JW_OK, or JW_INVALID (not that extent's size,
   or none left) or JW_FULL (no room for its run), nothing changed.  */
extern int __jw_grow (struct jw_table *t, uint32_t pages);

/* A slice of PAGES, the first free run's first pages that hold it: its
   first page in *PAGE.  JW_INVALID (no pages), JW_FULL (slices), JW_NONE
   (no free run that long: grow first); nothing changed then.  */
extern int __jw_take (struct jw_table *t, uint32_t pages, uint32_t *page);

/* The slice [PAGE, PAGE + PAGES) given back, merged with the free runs
   it touches: JW_OK, or JW_INVALID (no pages, past the mapped part, any
   of it free already, or no slice out) or JW_FULL (no room for its run),
   nothing changed.  */
extern int __jw_give (struct jw_table *t, uint32_t page, uint32_t pages);

/* 0 if T is well formed (the header comment), else which rule it
   breaks.  */
extern int __jw_check (const struct jw_table *t);

#endif
