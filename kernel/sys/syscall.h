/* syscall.h - Phase 2's debug system calls (docs/phase2-plan.md step 6).
 *
 * Two calls, numbered in rax with arguments in rdi, rsi (the 0001
 * amendment's register shape, which Phase 3 extends): debug_write(ptr, len)
 * writes len bytes of the caller's memory to the serial console, and
 * exit(code) ends the task. Everything else fails. The result comes back in
 * rax: for debug_write the number of bytes written, otherwise 0, or the
 * negated SYS_E_* code. These are temporary: Phase 3 brings in docs/abi.md's
 * calls and result codes.
 *
 * The dispatcher is plain C with no CPU access (output goes through a
 * callback), so it is host-tested and fuzzed (§8.1). */
#ifndef JOIST_SYS_SYSCALL_H
#define JOIST_SYS_SYSCALL_H

#include "usercopy.h"

#include <stddef.h>
#include <stdint.h>

#define SYS_DEBUG_WRITE 1u
#define SYS_EXIT        2u

#define SYS_E_RANGE 1u /* a pointer outside the caller's readable user memory */
#define SYS_E_INVAL 2u /* an argument out of range (debug_write longer than the bound) */
#define SYS_E_NOSYS 3u /* no such call */

#define DEBUG_WRITE_MAX 4096u /* bytes per debug_write: bounds the call's work */

struct sys_env {
    const struct vm_space *task;                          /* the caller's address space */
    void (*out)(void *ctx, const uint8_t *buf, size_t n); /* where debug_write goes */
    void *ctx;
    int exited;         /* set by exit */
    uint64_t exit_code;
};

/* Decode and run one call. Never faults on bad arguments. */
uint64_t sys_dispatch(struct sys_env *env, uint64_t nr, uint64_t a0, uint64_t a1);

#endif /* JOIST_SYS_SYSCALL_H */
