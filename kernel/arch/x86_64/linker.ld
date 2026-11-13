OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2));
    rodata  PT_LOAD    FLAGS((1 << 2));
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2));
}

/* Each segment starts on its own 4 KiB page so the VMM can map it with
 * distinct permissions: text R-X, rodata R--, data/bss RW- (see vmm.c).
 * The __*_start/__*_end symbols delimit those ranges. */
SECTIONS
{
    . = 0xffffffff80200000;

    __text_start = .;
    .limine_reqs_start : { KEEP(*(.limine_reqs_start)) } :text
    .limine_reqs       : { KEEP(*(.limine_reqs)) }       :text
    .limine_reqs_end   : { KEEP(*(.limine_reqs_end)) }   :text

    .text : { *(.text .text.*) } :text
    __text_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __rodata_start = .;
    .rodata : { *(.rodata .rodata.*) } :rodata
    __rodata_end = .;

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __data_start = .;
    .data : { *(.data .data.*) } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data
    . = ALIGN(CONSTANT(MAXPAGESIZE));
    __kernel_end = .;

    /DISCARD/ : { *(.eh_frame*) *(.note.*) *(.comment) }
}
