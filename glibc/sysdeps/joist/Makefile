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

# The task's address reservations (joist-mem.c, which includes joist-vm.c):
# one table in a program, as the name table.
ifeq ($(subdir),mach)
routines += joist-mem
endif
ifeq ($(subdir),elf)
sysdep-rtld-routines += joist-mem
endif
