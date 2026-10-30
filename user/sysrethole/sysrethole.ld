/* SPDX-License-Identifier: GPL-3.0-only */
/* sysrethole.ld - one code page, the last of the user half. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); }
SECTIONS
{
    . = 0x7ffffffff000;
    .text : { *(.text) } :text
    /DISCARD/ : { *(.note .note.*) *(.comment) }
}
