/* SPDX-License-Identifier: GPL-3.0-only */
/* run.h - the kernel at work: the core's state, address spaces, threads and
 * the way into and out of user mode (Phase 3 step 4). Replaces Phase 2's
 * single root task (user.c). */
#ifndef JOIST_ARCH_RUN_H
#define JOIST_ARCH_RUN_H

#include "cpu.h"
#include "frame.h"

#include <limine/limine.h>
#include <stdint.h>

/* The root task's start, as its registers tell it (0013), in the C calling
 * convention's argument order so its entry can be a C function: RDI the
 * first of RSI memory frames (RUN_POOL_FRAMES of RUN_POOL_PAGES pages each,
 * 2 MiB aligned), RDX the first of RCX boot-module frames (modules 1
 * onwards, in limine.conf's order; module 0 is the root task itself), R8 its
 * first free slot, R9 the number of CPUs in use. Its own image and stack are frames in leaf 1 (slots 128-255): its
 * to keep. By convention, not registers: its own task capability is in slot
 * RUN_ROOT_SELF (the only way it maps into itself, 0020), and slot
 * RUN_ROOT_TABLES holds the frame of RUN_ROOT_TABLE_PAGES pages already lent
 * to its space for its page tables (0021). */
#define RUN_ROOT_SC      0u /* its own scheduling context */
#define RUN_POOL_FIRST   1u
#ifndef RUN_POOL_FRAMES
#define RUN_POOL_FRAMES  4u
#endif
#ifndef RUN_POOL_PAGES
#define RUN_POOL_PAGES   512u /* (larger for mm's benchmark of large clones) */
#endif
#define RUN_ROOT_SELF    5u /* its own task capability (joist_boot_self) */
#define RUN_ROOT_TABLES  6u /* a frame lent to its own space for its page tables (0021) */
#define RUN_ROOT_TABLE_PAGES 256u
#define RUN_MODULE_FIRST 8u
#define RUN_FIRST_FREE   16u
#define RUN_IMAGE_FIRST  128u

/* Set everything up from the bootloader's answers and run the root task
 * (module 0). Returns only on failure, with what failed. */
const char *run_start(struct frame_alloc *fa, uint64_t hhdm,
                      const struct limine_memmap_response *mm,
                      const struct limine_module_response *mods,
                      const struct limine_mp_response *mp);

/* An interrupt or exception from user mode, or a system call: never returns
 * (the kernel goes back to user mode with resume_user). idt.c. */
__attribute__((noreturn)) void run_user_trap(struct trap_frame *tf);

/* A timer interrupt or one from another CPU, taken in kernel mode: while
 * the boot waits for its ticks, or while a CPU sleeps. */
void run_kernel_irq(uint64_t vector);

/* Self-test 8 (boot.c): overflow a kernel stack into its guard page. */
void run_selftest_stack_guard(void);

#endif /* JOIST_ARCH_RUN_H */
