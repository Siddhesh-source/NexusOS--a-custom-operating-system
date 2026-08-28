OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2));
    rodata  PT_LOAD    FLAGS((1 << 2));
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2));
}

SECTIONS
{
    . = 0xffffffff80200000;

    .limine_reqs_start : { KEEP(*(.limine_reqs_start)) } :text
    .limine_reqs       : { KEEP(*(.limine_reqs)) }       :text
    .limine_reqs_end   : { KEEP(*(.limine_reqs_end)) }   :text

    .text : { *(.text .text.*) } :text

    . += CONSTANT(MAXPAGESIZE) * 2;
    .rodata : { *(.rodata .rodata.*) } :rodata

    . += CONSTANT(MAXPAGESIZE) * 2;
    .data : { *(.data .data.*) } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    } :data

    /DISCARD/ : { *(.eh_frame*) *(.note.*) *(.comment) }
}
