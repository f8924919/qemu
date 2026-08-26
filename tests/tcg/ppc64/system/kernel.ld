/* SPDX-License-Identifier: GPL-2.0-or-later */
ENTRY(_start)

SECTIONS
{
    /*
     * The image is loaded with -bios, so it lives in the read-only ROM
     * region and the 970 starts executing at PROM_BASE + 0x100.
     */
    . = 0xfff00100;
    _text = .;
    .text : {
        KEEP(*(.text.boot))
        *(.text)
        *(.text.*)
    }
    . = ALIGN(8);
    _etext = .;

    /*
     * Everything else goes to RAM, which -bios loads directly: it honours
     * p_paddr for every segment, so nothing has to be copied at run time.
     * Read-only data comes along rather than staying in ROM, because it is
     * addressed TOC-relative and a 16-bit displacement cannot reach from
     * RAM to the ROM region.
     */
    . = 0x00100000;
    .rwdata : {
        _rwdata = .;
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
        *(.sdata)
        *(.sdata.*)
        . = ALIGN(8);
        *(.got)
        *(.toc)
        . = ALIGN(8);
        _erwdata = .;
    }

    .bss : {
        _bss = .;
        *(.bss)
        *(.bss.*)
        *(.sbss)
        *(.sbss.*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = .;
    }

    . = ALIGN(16);
    . = . + 0x10000;
    stack_end = .;

    /* nothing may sit in front of _start, and none of this is used here */
    /DISCARD/ : {
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.comment)
    }
}
