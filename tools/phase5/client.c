/* SPDX-License-Identifier: GPL-3.0-only */
/* client.c - Phase 5's test client for "hello", for the glibc inventory:
 * a lookup and the io_* calls the exit test makes, through glibc's Hurd
 * layer and MIG user stubs. */
#include <hurd.h>
#include <hurd/io.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    file_t f = file_name_lookup(argc > 1 ? argv[1] : "/hello", O_READ, 0);
    if (f == MACH_PORT_NULL) {
        perror("file_name_lookup");
        return 1;
    }
    char buf[256];
    char *data = buf;
    mach_msg_type_number_t n = sizeof buf;
    if (io_read(f, &data, &n, -1, sizeof buf) != 0) {
        return 2;
    }
    fwrite(data, 1, n, stdout);
    off_t off;
    struct stat st;
    vm_size_t wrote;
    return io_seek(f, 0, SEEK_SET, &off) != 0 || io_stat(f, &st) != 0 ||
           io_write(f, "x", 1, -1, &wrote) == 0;
}
