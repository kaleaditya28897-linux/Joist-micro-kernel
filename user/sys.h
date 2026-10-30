/* SPDX-License-Identifier: GPL-3.0-only */
/* sys.h - Phase 2's debug system calls, from user mode (kernel/sys/syscall.h
 * has the kernel side). Number in rax, arguments in rdi and rsi; the
 * `syscall` instruction clobbers rcx and r11. */
#ifndef JOIST_USER_SYS_H
#define JOIST_USER_SYS_H

#include <stdint.h>

#define SYS_DEBUG_WRITE 1u
#define SYS_EXIT        2u

static inline uint64_t sys2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    uint64_t r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return r;
}

static inline uint64_t debug_write(const void *p, uint64_t n)
{
    return sys2(SYS_DEBUG_WRITE, (uint64_t)(uintptr_t)p, n);
}

static inline __attribute__((noreturn)) void sys_exit(uint64_t code)
{
    sys2(SYS_EXIT, code, 0);
    for (;;) {
    }
}

#endif /* JOIST_USER_SYS_H */
