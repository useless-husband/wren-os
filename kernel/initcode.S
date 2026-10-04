/* The first process's code: exec("/bin/init").  Built as a user program
 * linked at USER_MIN and embedded in the kernel (see Makefile). */
    .section .rodata
    .balign 16
    .globl initcode_start, initcode_end
initcode_start:
    .incbin "build/user/initcode.bin"
initcode_end:
