/* SPDX-License-Identifier: GPL-3.0-only */
/* usercopy.c - reading and writing user memory through the page tables. See
 * usercopy.h. */
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

kerr copy_to_user(const struct vm_space *task, uint64_t uva, const void *src, size_t len)
{
    const uint8_t *in = src;
    if (len == 0) {
        return KERR_OK;
    }
    if (uva >= USER_TOP || USER_TOP - uva < (uint64_t)len) {
        return KERR_RANGE;
    }
    /* All of it or none: every page is checked before a byte is written. */
    for (uint64_t page = uva & ~(VM_PAGE - 1u); page < uva + len; page += VM_PAGE) {
        uint64_t pa;
        unsigned perms;
        if (vm_translate(task, page, &pa, &perms) != KERR_OK ||
            (perms & (VM_USER | VM_W)) != (VM_USER | VM_W)) {
            return KERR_RANGE;
        }
    }
    while (len != 0) {
        uint64_t pa;
        unsigned perms;
        if (vm_translate(task, uva, &pa, &perms) != KERR_OK) {
            return KERR_RANGE; /* checked above: not reached */
        }
        uint64_t in_page = VM_PAGE - (uva & (VM_PAGE - 1u));
        size_t n = len < in_page ? len : (size_t)in_page;
        uint8_t *dst = task->phys_to_ptr(task->ctx, pa);
        for (size_t i = 0; i < n; i++) {
            dst[i] = in[i];
        }
        in += n;
        uva += n;
        len -= n;
    }
    return KERR_OK;
}
