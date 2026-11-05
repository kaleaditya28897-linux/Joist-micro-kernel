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

/* mm maps where it is told (decision 0023), so libc chooses the address
   of every vm_allocate and anonymous vm_map: this table holds the task's
   reservations, sorted, none overlapping, each with its protection.  An
   "anywhere" request takes the first free run that fits in a fixed window
   (JV_LO to JV_HI); a fixed one any free run in the user half.  The same
   table answers vm_region.  Pages throughout, not bytes.

   Every call that changes the table has a check (__jv_can_*) that says,
   without changing anything, whether it will succeed: the caller checks,
   asks mm, and changes the table only once mm has done its part, so a
   refusal anywhere leaves the table and the memory as they were.
   Bounded: JV_MAX reservations; each call is a scan of them.  */

#ifndef _JOIST_VM_H
#define _JOIST_VM_H 1

#include <stdint.h>

#ifndef JV_MAX
# define JV_MAX 1024u
#endif
/* The user half: 2^47 bytes (Joist's JOIST_USER_PAGES).  */
#ifndef JV_USER_PAGES
# define JV_USER_PAGES (1ull << 35)
#endif
/* The window for "anywhere": 4 GiB to 64 TiB.  */
#ifndef JV_LO
# define JV_LO (1ull << 20)
#endif
#ifndef JV_HI
# define JV_HI (1ull << 34)
#endif

/* Results: Mach's (mach/kern_return.h).  */
#define JV_OK			0
#define JV_INVALID_ADDRESS	1
#define JV_NO_SPACE		3
#define JV_INVALID_ARGUMENT	4
#define JV_RESOURCE_SHORTAGE	6

struct jv_res
{
  uint64_t page, pages;
  uint32_t prot;
};

struct jv_table
{
  uint32_t n;
  struct jv_res r[JV_MAX];
};

extern void __jv_init (struct jv_table *t);

/* A free run of PAGES in the window, the lowest starting on a multiple of
   ALIGN (a power of 2: vm_map's mask): its first page.  */
extern int __jv_find (const struct jv_table *t, uint64_t pages,
		      uint64_t align, uint64_t *page);

/* [PAGE, PAGE + PAGES) reserved with PROT: it must be in the user half,
   past page 0, and free.  */
extern int __jv_can_reserve (const struct jv_table *t, uint64_t page,
			     uint64_t pages);
extern void __jv_reserve (struct jv_table *t, uint64_t page, uint64_t pages,
			  uint32_t prot);

/* Whatever of [PAGE, PAGE + PAGES) is reserved, released (a reservation
   cut in two if the range is inside it).  */
extern int __jv_can_release (const struct jv_table *t, uint64_t page,
			     uint64_t pages);
extern void __jv_release (struct jv_table *t, uint64_t page, uint64_t pages);

/* [PAGE, PAGE + PAGES), which must be reserved throughout, given PROT.  */
extern int __jv_can_protect (const struct jv_table *t, uint64_t page,
			     uint64_t pages);
extern void __jv_protect (struct jv_table *t, uint64_t page, uint64_t pages,
			  uint32_t prot);

/* vm_region: the first reservation that ends past PAGE, in *R.  */
extern int __jv_region (const struct jv_table *t, uint64_t page,
			struct jv_res *r);

/* The vm_* calls (bytes, and Mach's VM_PROT_* bits), on the table and mm.
   Each checks, then asks mm, then changes the table: a refusal from
   either changes nothing.  W and X together are refused
   (KERN_PROTECTION_FAILURE): mm maps W^X.  */
#define JV_PAGE_SHIFT 12
#define JV_PROT_READ 1u
#define JV_PROT_WRITE 2u
#define JV_PROT_EXECUTE 4u
#define JV_PROTECTION_FAILURE 2
#define JV_FAILURE 5

/* vm_allocate and anonymous vm_map: *ADDR where (rounded down to a page)
   unless ANYWHERE, and where it went; SIZE rounded up to pages; MASK, the
   alignment less one (vm_map's); PROT the protection.  */
extern int __jv_allocate (struct jv_table *t, uint64_t *addr, uint64_t size,
			  uint64_t mask, int anywhere, uint32_t prot);
/* vm_deallocate: whatever is mapped in the pages of [ADDR, ADDR + SIZE).  */
extern int __jv_deallocate (struct jv_table *t, uint64_t addr,
			    uint64_t size);
/* vm_protect (the current protection).  */
extern int __jv_vm_protect (struct jv_table *t, uint64_t addr, uint64_t size,
			    uint32_t prot);
/* vm_region: the first reservation that ends past *ADDR: its address,
   size and protection.  */
extern int __jv_vm_region (const struct jv_table *t, uint64_t *addr,
			   uint64_t *size, uint32_t *prot);

/* The environment: mm's requests on this task (sysdeps/joist; a fake in
   the host tests).  0, or mm's result (MM_E_*: 1 a bad range, 2 no room, 3
   in the way of something mm holds, 4 the kernel refused).  */
#define JV_MM_MAP_ANON 1
#define JV_MM_UNMAP 2
#define JV_MM_PROTECT 3
#define JV_MM_W 1u
#define JV_MM_X 2u
#define JV_MM_NONE 4u
extern int __joist_vm_mm (int req, uint64_t page, uint64_t pages,
			  uint32_t perms);

/* 0 if T is well formed (sorted, none empty or overlapping, all in the
   user half past page 0), else which rule it breaks.  */
extern int __jv_check (const struct jv_table *t);

#endif
