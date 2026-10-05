/* usercopy.c - reading user memory through the page tables. See usercopy.h. */
#include "usercopy.h"

kerr copy_from_user(const struct vm_space *task, void *dst, uint64_t uva, size_t len)
{
    uint8_t *out = dst;
    if (len == 0) {
        return KERR_OK;
    }
    if (uva >= USER_TOP || (uint64_t)len > USER_TOP - uva) {
        return KERR_RANGE;
    }
    while (len != 0) {
        uint64_t pa;
        unsigned perms;
        if (vm_translate(task, uva, &pa, &perms) != KERR_OK || (perms & VM_USER) == 0) {
            return KERR_RANGE;
        }
        uint64_t in_page = VM_PAGE - (uva & (VM_PAGE - 1u));
        size_t n = len < in_page ? len : (size_t)in_page;
        /* pa .. pa + n stays inside one page, so one pointer covers it. */
        const uint8_t *src = task->phys_to_ptr(task->ctx, pa);
        for (size_t i = 0; i < n; i++) {
            out[i] = src[i];
        }
        out += n;
        uva += n;
        len -= n;
    }
    return KERR_OK;
}
