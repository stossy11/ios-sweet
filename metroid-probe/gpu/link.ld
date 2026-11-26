ENTRY(_head)
SECTIONS
{
    . = 0;
    .text : { *(.text.head) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .got : { *(.got*) }
    /* PIE callbacks/constant pointer tables are relocated before entering C.
     * Builder rejects any dynamic relocation other than AArch64 RELATIVE. */
    .rela.dyn : { _rela_start = .; *(.rela.dyn) _rela_end = .; }
    .bss : { _bss_start = .; *(.bss*) *(COMMON) _bss_end = .; }
    . = ALIGN(16);
    . += 0x20000;
    _stack_top = .;
    .pad : { BYTE(0) }
    _image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
