OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

PHDRS
{
    rodata  PT_LOAD FLAGS(4); /* PF_R */
    text    PT_LOAD FLAGS(5); /* PF_R | PF_X */
    data    PT_LOAD FLAGS(6); /* PF_R | PF_W */
    dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
    /*
     * GNU ld -pie forces e_type = ET_EXEC if the lowest PT_LOAD segment starts
     * at p_vaddr != 0. Put .rodata (PF_R) at vaddr 0 with a 16-byte sentinel so
     * PT_LOAD[0] is at vaddr 0 (preserving ET_DYN), while every real symbol in
     * .rodata/.text/.data keeps st_value != 0 (required by pxe.c relocator).
     */
    . = 0;

    .rodata : ALIGN(0x1000) {
        QUAD(0x4F4B454E5F505845);
        QUAD(0x0000000100000000);
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

    .text : ALIGN(0x1000) {
        *(.text .text.*)
        *(.plt .plt.* .iplt)
    } :text

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
