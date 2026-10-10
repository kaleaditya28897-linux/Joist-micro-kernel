/* usercopy.h - the only way the kernel reads or writes user memory (Phase 2 step 6;
 * §5.1: "user pointers are never dereferenced directly").
 *
 * copy_from_user never touches the user's virtual address: it walks the
 * task's page tables in software (vm_translate), requires every byte to lie
 * in the user half and on a page mapped user-accessible, and copies through
 * the physical address (the direct map in the kernel; simulated memory in the
 * host tests). A bad pointer is an error code, never a kernel page fault, and
 * there is no window between check and use on one CPU. Bounded: one walk per
 * page of the range; the caller bounds the length. */
#ifndef JOIST_SYS_USERCOPY_H
#define JOIST_SYS_USERCOPY_H

#include "vm.h"

#include <stddef.h>
#include <stdint.h>

/* The top of the user half (exclusive): canonical lower half. */
#define USER_TOP 0x0000800000000000ull

/* Copy [uva, uva + len) of the task's memory to dst. KERR_RANGE if any of it
 * lies outside the user half, wraps, or is not mapped user-accessible; then
 * dst may hold part of the data. len 0 always succeeds. */
kerr copy_from_user(const struct vm_space *task, void *dst, uint64_t uva, size_t len);

/* Copy len bytes from src to [uva, uva + len) of the task's memory (Phase 3
 * step 6). KERR_RANGE on the same conditions, or if any page of it isn't
 * mapped user-writable; then nothing was written. */
kerr copy_to_user(const struct vm_space *task, uint64_t uva, const void *src, size_t len);

#endif /* JOIST_SYS_USERCOPY_H */
