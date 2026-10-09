/* run.h - the kernel at work: the core's state, address spaces, threads and
 * the way into and out of user mode (Phase 3 step 4). Replaces Phase 2's
 * single root task (user.c). */
#ifndef JOIST_ARCH_RUN_H
#define JOIST_ARCH_RUN_H

#include "cpu.h"
#include "frame.h"

#include <limine/limine.h>
#include <stdint.h>

/* The root task's start, as its registers tell it (0013): RDI the first of
 * RSI memory frames (RUN_POOL_FRAMES of RUN_POOL_PAGES pages each, 2 MiB
 * aligned), RDX the first of R10 boot-module frames (modules 1 onwards, in
 * limine.conf's order; module 0 is the root task itself), R8 its first free
 * slot. Its own image and stack are frames in leaf 1 (slots 128-255): its
 * to keep. */
#define RUN_POOL_FIRST   1u
#define RUN_POOL_FRAMES  4u
#define RUN_POOL_PAGES   512u
#define RUN_MODULE_FIRST 8u
#define RUN_FIRST_FREE   16u
#define RUN_IMAGE_FIRST  128u

/* Set everything up from the bootloader's answers and run the root task
 * (module 0). Returns only on failure, with what failed. */
const char *run_start(struct frame_alloc *fa, uint64_t hhdm,
                      const struct limine_memmap_response *mm,
                      const struct limine_module_response *mods);

/* An interrupt or exception from user mode, or a system call: never returns
 * (the kernel goes back to user mode with resume_user). idt.c. */
__attribute__((noreturn)) void run_user_trap(struct trap_frame *tf);

/* A timer tick, from user or kernel mode. */
void run_tick(void);

#endif /* JOIST_ARCH_RUN_H */
