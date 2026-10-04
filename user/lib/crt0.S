/* Program entry: the kernel's exec leaves argc in x0, argv in x1 and a
 * 16-byte aligned stack. */
    .section .text.start, "ax"
    .globl _start
_start:
    mov     x29, #0
    mov     x30, #0
    bl      main
    bl      exit
1:  b       1b
