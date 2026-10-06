/* void roots_with_registers(void (*fn)(void *ctx, void *sp), void *ctx)
 *
 * Spill every callee-saved register (x19-x28, fp, lr and the low halves of
 * v8-v15, d8-d15) onto the stack, then call fn(ctx, sp) with sp pointing at
 * the spill area.  A pointer that the caller keeps only in a register is
 * then in memory between sp and the stack top, where the root scan finds
 * it.  Caller-saved registers (x0-x18, v0-v7, v16-v31) need no spilling:
 * by the AAPCS64 they are dead across this call, so the compiler has
 * already saved any live value in them to the stack.
 *
 * The area is exactly 160 bytes and every slot is written, so no stale
 * word from an earlier, deeper call can hide in it. */
    .text
    .globl roots_with_registers
    .type roots_with_registers, %function
roots_with_registers:
    stp     x29, x30, [sp, #-160]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    stp     d8, d9, [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]
    mov     x2, x0
    mov     x0, x1
    mov     x1, sp
    blr     x2                  /* fn preserves x19-x28 and d8-d15 itself */
    ldp     x29, x30, [sp], #160
    ret
    .size roots_with_registers, . - roots_with_registers
