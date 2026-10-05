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

    __image_start = .;
    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __text_start = .;
    .text : { *(.text .text.*) } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_start = .;
    .rodata : { *(.rodata .rodata.*) } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_start = .;
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    __data_end = .;

    /DISCARD/ : { *(.eh_frame*) *(.note .note.*) *(.comment) }
}
