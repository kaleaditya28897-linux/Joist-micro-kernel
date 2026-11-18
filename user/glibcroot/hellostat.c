/* SPDX-License-Identifier: GPL-3.0-only */
/* hellostat.c - what glibcroot (a raw program, without glibc's headers)
 * reads of the struct stat io_stat returns (Phase 5 step 10): built with
 * glibc's headers by tools/phase5/hello.sh into OUT.helloroot.o. */
#include <sys/stat.h>
#include <stddef.h>
#include <string.h>

/* The bytes of glibc's struct stat (io_statbuf_t), and two of its fields. */
unsigned long hr_stat_bytes(void)
{
    return sizeof(struct stat);
}
long long hr_stat_size(const void *st)
{
    return (long long)((const struct stat *)st)->st_size;
}
unsigned int hr_stat_mode(const void *st)
{
    return (unsigned int)((const struct stat *)st)->st_mode;
}

/* The root's node (rootnode.defs' io_stat): a regular file, readable by
 * everyone, of `size` bytes; the rest zero. */
void hr_stat_fill(void *st, long long size)
{
    struct stat *s = st;
    memset(s, 0, sizeof *s);
    s->st_mode = S_IFREG | 0444;
    s->st_size = size;
    s->st_nlink = 1;
    s->st_blksize = 4096;
}
