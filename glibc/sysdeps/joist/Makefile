# Joist's Mach layer (Phase 5 step 7): what sysdeps/joist adds to the
# build.  The files that replace the Mach layer's are found by name.
# Copyright (C) 2026 Aditya Kale.
# This file is part of the GNU C Library.
#
# The GNU C Library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# The GNU C Library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with the GNU C Library; if not, see
# <https://www.gnu.org/licenses/>.

# The port name table: joist-ports.c, which includes joist-names.c.  A
# program has one: ld.so's if it is dynamic, libc.a's if it is static
# (libc.so's joist-ports.o is empty).
ifeq ($(subdir),mach)
routines += joist-ports
endif
ifeq ($(subdir),elf)
sysdep-rtld-routines += joist-ports
endif

# MIG's runtime (joist-mig.c): one in a program, as the name table.
ifeq ($(subdir),mach)
routines += joist-mig
endif
ifeq ($(subdir),elf)
sysdep-rtld-routines += joist-mig
endif

# Threads' notifications and the futex queues (joist-sync.c, which
# includes joist-gsync.c): one in a program, as the name table.
ifeq ($(subdir),mach)
routines += joist-sync
endif
ifeq ($(subdir),elf)
sysdep-rtld-routines += joist-sync
endif

# The bootstrap port (joist-special.c): one in a program, as the table.
ifeq ($(subdir),mach)
routines += joist-special
endif
ifeq ($(subdir),elf)
sysdep-rtld-routines += joist-special
endif

# The task's address reservations (joist-mem.c, which includes joist-vm.c):
# one table in a program, as the name table.
ifeq ($(subdir),mach)
routines += joist-mem
endif
ifeq ($(subdir),elf)
sysdep-rtld-routines += joist-mem
endif
# A static program's first vm_allocate comes before its IFUNCs are
# resolved: no library calls the compiler makes up of its loops.
CFLAGS-joist-mem.c += -fno-builtin
# And the pid asked for at start-up (joist-special.c), the same way.
CFLAGS-joist-special.c += -fno-builtin
