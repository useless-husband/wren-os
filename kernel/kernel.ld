/* The kernel is linked at KIMAGE_VBASE and mapped there at run time,
 * wherever the loader placed it physically.  Sections are page aligned so
 * vm.c can map text read-only+executable and everything else non-executable. */
ENTRY(_start)

SECTIONS
{
    . = 0xffffffffc0000000;
    _start_image = .;

    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
    }
    . = ALIGN(4096);
    _etext = .;

    .rodata : {
        *(.rodata .rodata.*)
    }
    . = ALIGN(4096);
    _erodata = .;

    .data : {
        *(.data .data.*)
    }
    . = ALIGN(4096);
    _edata = .;

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        _bss_end = .;
    }
    . = ALIGN(4096);

    /* Boot page tables live outside .bss: they are in use while .bss is zeroed. */
    .pgtbl (NOLOAD) : {
        *(.pgtbl)
    }
    . = ALIGN(4096);
    _end = .;

    _image_size = _end - _start_image;

    /DISCARD/ : {
        *(.comment) *(.note*) *(.eh_frame*)
    }
}
