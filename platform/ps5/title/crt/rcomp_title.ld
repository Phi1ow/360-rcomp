/*
 * R-comp PS5 title: layout of the intermediate PIE (owner: Agent 2).
 *
 * Written for this project from the input constraints of the ELF->PS5
 * converter (`ps5-native-tool link`, see platform/ps5/README.md):
 *   - .text starts at virtual address 0, file offset >= one 16 KiB page;
 *   - code (R-X), read-only data (R--), RELRO (RW-, holds .got/.dynsym/...)
 *     and writable data (RW-, starting with .dynamic) are separate PT_LOADs;
 *   - a page-aligned gap between RELRO and data, where the converter writes
 *     the PS5 process parameters;
 *   - no symbol is exported (a version script makes everything local).
 * No segment is both writable and executable.
 *
 * Symbols provided for platform/ps5/title/crt/rcomp_title_crt.cpp:
 *   __rcomp_bss_begin/__rcomp_bss_end         BSS the loader may leave dirty
 *   __rcomp_{preinit,init,fini}_array_{begin,end}
 * and for the SDK's libunwind (it locates unwind tables through these instead
 * of dl_iterate_phdr): __eh_frame_{start,end}, __eh_frame_hdr_{start,end}.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)

PHDRS {
    seg_code  PT_LOAD FLAGS(5);     /* R-X */
    seg_const PT_LOAD FLAGS(4);     /* R-- */
    seg_relro PT_LOAD FLAGS(6);     /* RW- until the loader applies RELRO */
    seg_data  PT_LOAD FLAGS(6);     /* RW- */
    seg_dyn   PT_DYNAMIC FLAGS(6);
}

SECTIONS {
    .text 0 : ALIGN(CONSTANT(MAXPAGESIZE)) {
        /* int3 padding: address 0 is never a valid function entry */
        QUAD(0xcccccccccccccccc)
        QUAD(0xcccccccccccccccc)
        *(.text.rcomp_entry)
        *(.text .text.*)
        *(.plt .plt.*)
    } :seg_code

    /* Relocated RX bounds for the CPU sampler; weak readers fail closed. */
    PROVIDE_HIDDEN(__rcomp_text_begin = ADDR(.text));
    PROVIDE_HIDDEN(__rcomp_text_end = ADDR(.text) + SIZEOF(.text));

    .eh_frame_hdr : ALIGN(CONSTANT(MAXPAGESIZE)) { KEEP(*(.eh_frame_hdr)) } :seg_const
    .eh_frame : { KEEP(*(.eh_frame)) } :seg_const
    .gcc_except_table : { *(.gcc_except_table .gcc_except_table.*) } :seg_const
    .rodata : { *(.rodata .rodata.*) } :seg_const
    PROVIDE_HIDDEN(__eh_frame_start = ADDR(.eh_frame));
    PROVIDE_HIDDEN(__eh_frame_end = ADDR(.eh_frame) + SIZEOF(.eh_frame));
    PROVIDE_HIDDEN(__eh_frame_hdr_start = ADDR(.eh_frame_hdr));
    PROVIDE_HIDDEN(__eh_frame_hdr_end = ADDR(.eh_frame_hdr) + SIZEOF(.eh_frame_hdr));

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :seg_relro
    .got : { *(.got) } :seg_relro
    .got.plt : { *(.got.plt) } :seg_relro
    .preinit_array : {
        PROVIDE_HIDDEN(__rcomp_preinit_array_begin = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__rcomp_preinit_array_end = .);
    } :seg_relro
    .init_array : {
        PROVIDE_HIDDEN(__rcomp_init_array_begin = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__rcomp_init_array_end = .);
    } :seg_relro
    .fini_array : {
        PROVIDE_HIDDEN(__rcomp_fini_array_begin = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__rcomp_fini_array_end = .);
    } :seg_relro
    /* Dynamic-link metadata: read and rewritten by the converter. */
    .dynsym : { *(.dynsym) } :seg_relro
    .gnu.hash : { *(.gnu.hash) } :seg_relro
    .hash : { *(.hash) } :seg_relro
    .dynstr : { *(.dynstr) } :seg_relro
    .rela.dyn : { *(.rela.dyn) } :seg_relro
    .rela.plt : { *(.rela.plt) } :seg_relro

    . = ALIGN(CONSTANT(MAXPAGESIZE));
    .dynamic : { *(.dynamic) } :seg_data :seg_dyn
    .data : { *(.data .data.*) } :seg_data
    .tdata : { *(.tdata .tdata.*) } :seg_data
    .tbss : { *(.tbss .tbss.*) } :seg_data
    .bss : {
        PROVIDE_HIDDEN(__rcomp_bss_begin = .);
        *(.bss .bss.*)
        *(COMMON)
        PROVIDE_HIDDEN(__rcomp_bss_end = .);
    } :seg_data

    /DISCARD/ : { *(.interp) *(.note.GNU-stack) }
}
