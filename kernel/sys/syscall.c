/* syscall.c - Phase 2's debug system calls. See syscall.h. */
#include "syscall.h"

static uint64_t fail(uint64_t code)
{
    return (uint64_t)0 - code;
}

/* One buffer: Phase 2 runs one task on one CPU, and the call copies the
 * whole message before writing any of it. */
static uint8_t buf[DEBUG_WRITE_MAX];

uint64_t sys_dispatch(struct sys_env *env, uint64_t nr, uint64_t a0, uint64_t a1)
{
    switch (nr) {
    case SYS_DEBUG_WRITE:
        if (a1 > DEBUG_WRITE_MAX) {
            return fail(SYS_E_INVAL);
        }
        /* All of it is checked and copied before any is written, so a bad
         * pointer produces no output at all. */
        if (copy_from_user(env->task, buf, a0, (size_t)a1) != KERR_OK) {
            return fail(SYS_E_RANGE);
        }
        env->out(env->ctx, buf, (size_t)a1);
        return a1;
    case SYS_EXIT:
        env->exited = 1;
        env->exit_code = a0;
        return 0;
    default:
        return fail(SYS_E_NOSYS);
    }
}
