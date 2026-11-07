/* _hurd_fd_write -- write to a file descriptor; handles job control et al.
   Copyright (C) 1993-2026 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

/* Joist's copy of hurd/fd-write.c (Phase 5 step 7), changed in one place:
   one io_write passes at most JOIST_IO_WRITE_MAX bytes, what a message
   carries in-line on Joist (joist_mig.h; out-of-line data is later).  A
   longer write is a short write, which write's callers, stdio among
   them, go on from.  */

#include <errno.h>
#include <unistd.h>
#include <hurd.h>
#include <hurd/fd.h>

/* A body is at most JOIST_MIG_WORDS words; io_write's takes the count and
   the offset besides the bytes.  */
#define JOIST_IO_WRITE_MAX 4096u

/* The write, and what it wrote.  Not a nested function in _hurd_fd_write,
   which clang lacks.  */
struct writefd
{
  const void *buf;
  size_t nbytes;
  loff_t offset;
  vm_size_t wrote;
};

static error_t
writefd (void *ctx, io_t port)
{
  struct writefd *w = ctx;
  size_t n = w->nbytes < JOIST_IO_WRITE_MAX ? w->nbytes : JOIST_IO_WRITE_MAX;
  return __io_write (port, w->buf, n, w->offset, &w->wrote);
}

error_t
_hurd_fd_write (struct hurd_fd *fd,
		const void *buf, size_t *nbytes, loff_t offset)
{
  error_t err;
  struct writefd w = { buf, *nbytes, offset, 0 };

  err = HURD_FD_PORT_USE_CANCEL (fd, _hurd_ctty_output (port, ctty,
							writefd, &w));
  vm_size_t wrote = w.wrote;
  if (err)
    return err;

  if (__glibc_unlikely (wrote > *nbytes))	/* Sanity check for bogus server.  */
    return EGRATUITOUS;

  *nbytes = wrote;

  return 0;
}
