/* linker.ld - the kernel image (Phase 2 step 1). Linked at the top 2 GiB
 * (-mcmodel=kernel), with one segment per permission so W^X holds from the
 * first instruction: code R+X, read-only data R, data and BSS R+W. Limine
 * maps each PT_LOAD with its flags. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

/* Flags: 4 = R, 2 = W, 1 = X. The requests segment is R+W because Limine
 * writes its responses there. */
PHDRS
{
    limine_requests PT_LOAD FLAGS(6);
    text            PT_LOAD FLAGS(5);
    rodata          PT_LOAD FLAGS(4);
    data            PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0xffffffff80000000;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .text : { *(.text .text.*) } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
