/* Program entry: the kernel's exec leaves argc in x0, argv in x1 and a
 * 16-byte aligned stack.  The initial sp is the top of every frame this
 * program will ever push (only argv lies above it), so it is recorded for
 * the conservative root scan (roots.c). */
    .section .text.start, "ax"
    .globl _start
_start:
    mov     x9, sp
    adrp    x10, __stack_top
    str     x9, [x10, :lo12:__stack_top]
    mov     x29, #0
    mov     x30, #0
    bl      main
    bl      exit
1:  b       1b

    .bss
    .balign 8
    .globl __stack_top
__stack_top:
    .quad   0
