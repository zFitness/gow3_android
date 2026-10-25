// void gportcpu_host_call(void *fn, const HostArgs *args, HostResult *result)
// HostArgs: x[8] at 0, stack_count at 64, stack at 72, v[8] at 80. HostResult: x[2] at 0, v[2] at 16.
// x8 also gets args->x[0]: a SysV callee returning a large structure takes its address in rdi.
    .text
    .globl gportcpu_host_call
    .type gportcpu_host_call, %function
    .p2align 4
gportcpu_host_call:
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, x2
    mov x16, x0
    mov x17, x1
    ldr x9, [x17, #64]
    ldr x10, [x17, #72]
    add x11, x9, #1
    and x11, x11, #~1
    sub sp, sp, x11, lsl #3
    mov x12, #0
1:  cmp x12, x9
    b.hs 2f
    ldr x13, [x10, x12, lsl #3]
    str x13, [sp, x12, lsl #3]
    add x12, x12, #1
    b 1b
2:  ldp q0, q1, [x17, #80]
    ldp q2, q3, [x17, #112]
    ldp q4, q5, [x17, #144]
    ldp q6, q7, [x17, #176]
    ldp x0, x1, [x17, #0]
    ldp x2, x3, [x17, #16]
    ldp x4, x5, [x17, #32]
    ldp x6, x7, [x17, #48]
    mov x8, x0
    blr x16
    stp x0, x1, [x19]
    stp q0, q1, [x19, #16]
    mov sp, x29
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
    .size gportcpu_host_call, .-gportcpu_host_call
    .section .note.GNU-stack,"",%progbits
