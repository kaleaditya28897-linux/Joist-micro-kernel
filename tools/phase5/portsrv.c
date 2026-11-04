/* SPDX-License-Identifier: GPL-3.0-only */
/* portsrv.c - a raw libports server for the glibc inventory (0031's
 * narrower scope: libports and a MIG server, no libtrivfs): one bucket,
 * one class, one port, a demuxer, served one thread at a time. */
#include <hurd.h>
#include <hurd/ports.h>
#include <mach.h>

static int demux(mach_msg_header_t *in, mach_msg_header_t *out)
{
    (void)in;
    (void)out;
    return 0;
}

int main(void)
{
    struct port_bucket *b = ports_create_bucket();
    struct port_class *c = ports_create_class(0, 0);
    void *p;
    if (ports_create_port(c, b, 64, &p) != 0) {
        return 1;
    }
    mach_port_t right = ports_get_right(p);
    (void)right;
    ports_manage_port_operations_one_thread(b, demux, 0);
    return 0;
}
