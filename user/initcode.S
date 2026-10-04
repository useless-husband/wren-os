/* First user-mode code (pid 1 before it execs): exec("/bin/init"). */
#include <wren/syscall.h>

    .text
    .globl _start
_start:
    adr     x0, init_path
    adr     x1, init_argv
    mov     x8, #SYS_exec
    svc     #0
    mov     x0, #127                /* exec failed */
    mov     x8, #SYS_exit
    svc     #0
1:  b       1b

    .balign 8
init_argv:
    .quad   init_path               /* absolute: this code is linked at its run address */
    .quad   0
init_path:
    .asciz  "/bin/init"
