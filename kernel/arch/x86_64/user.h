/* user.h - the root task: loading it and running it in ring 3 (Phase 2
 * step 6). */
#ifndef JOIST_ARCH_USER_H
#define JOIST_ARCH_USER_H

#include "frame.h"
#include "vm.h"

#include <stddef.h>
#include <stdint.h>

/* What syscall.S saves, lowest address first. */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r10, r9, r8, rdx, rsi, rdi, rax;
    uint64_t rcx; /* user RIP after the syscall instruction */
    uint64_t r11; /* user RFLAGS */
    uint64_t user_rsp;
};

/* Build the root task's address space from an ELF64 image (bytes reached
 * through the direct map) and enter it in ring 3. Returns only on failure,
 * with what failed. */
const char *user_run_root(struct frame_alloc *fa, uint64_t hhdm, const uint8_t *image,
                          uint64_t size);

uint64_t syscall_handler(struct syscall_frame *f);

#endif /* JOIST_ARCH_USER_H */
