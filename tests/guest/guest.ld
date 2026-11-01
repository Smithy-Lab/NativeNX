/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The layout of an NSO: text from address 0, then read-only data, then data and bss, each on its own page */
PHDRS { text PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); dyn PT_DYNAMIC FLAGS(6); }
SECTIONS {
    . = 0;
    .text : { KEEP(*(.text.crt0)) *(.text .text.*) } :text
    .plt : { *(.plt .plt.*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :ro
    .dynsym : { *(.dynsym) } :ro
    .dynstr : { *(.dynstr) } :ro
    .hash : { *(.hash) } :ro
    .gnu.hash : { *(.gnu.hash) } :ro
    .rela.dyn : { *(.rela.dyn) } :ro
    .rela.plt : { *(.rela.plt) } :ro
    .relr.dyn : { *(.relr.dyn) } :ro
    .eh_frame_hdr : { *(.eh_frame_hdr) } :ro
    .eh_frame : { *(.eh_frame) } :ro
    . = ALIGN(0x1000);
    .data : { *(.data .data.*) } :data
    .init_array : { KEEP(*(.init_array .init_array.*)) } :data
    .fini_array : { KEEP(*(.fini_array .fini_array.*)) } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .got : { *(.got) } :data
    .got.plt : { *(.got.plt) } :data
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    . = ALIGN(8);
    __bss_end = .;
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.interp) }
}
