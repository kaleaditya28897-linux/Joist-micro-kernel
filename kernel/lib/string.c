/* SPDX-License-Identifier: GPL-3.0-only */
/* string.c - the memory functions a freestanding Clang build may call even
 * with -fno-builtin (struct copies, large initialisers). Byte loops, but
 * for the copies forward: on x86-64 `rep movsb`, the CPU's fast string
 * copy, since decision 0032's reply_read and reply_write copy up to 64 KiB
 * under the kernel's lock (a byte loop took 1.2 cycles a byte). */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* Copy n bytes forward, from the first to the last. */
static void forward(unsigned char *d, const unsigned char *s, size_t n)
{
#if defined(__x86_64__)
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
#endif
}

void *memcpy(void *dst, const void *src, size_t n)
{
    forward(dst, src, n);
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s || d >= s + n) {
        forward(d, s, n); /* nothing of src overwritten before it is read */
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1u] = s[i - 1u];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a;
    const unsigned char *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}
