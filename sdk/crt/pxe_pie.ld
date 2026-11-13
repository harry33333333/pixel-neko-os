OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

PHDRS
{
    text    PT_LOAD FLAGS(5); /* PF_R | PF_X */
    rodata  PT_LOAD FLAGS(4); /* PF_R */
    data    PT_LOAD FLAGS(6); /* PF_R | PF_W */
    dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
    /* Keep base >= 0x1000 so no defined symbol ever has st_value == 0 */
    . = 0x1000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
        *(.plt .plt.* .iplt)
    } :text

    . = ALIGN(0x1000);

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    } :rodata

    .rela.dyn : {
        *(.rela.dyn)
        *(.rela.plt)
        *(.rela.iplt)
        *(.rela.text*)
        *(.rela.rodata*)
        *(.rela.data*)
    } :rodata

    .dynsym   : { *(.dynsym) } :rodata
    .dynstr   : { *(.dynstr) } :rodata
    .hash     : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata

    . = ALIGN(0x1000);

    .data : ALIGN(0x1000) {
        *(.data .data.*)
        *(.data.rel.ro .data.rel.ro.*)
    } :data

    .dynamic : { *(.dynamic) } :data :dynamic
    .got     : { *(.got) } :data
    .got.plt : { *(.got.plt) } :data

    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    } :data

    /DISCARD/ : {
        *(.eh_frame .eh_frame.*)
        *(.eh_frame_hdr)
        *(.sframe)
        *(.note .note.*)
        *(.comment)
        *(.interp)
    }
}
