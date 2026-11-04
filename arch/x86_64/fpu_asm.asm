; Pixel Neko OS - FPU/SSE state primitives

[bits 64]
default rel

global fpu_save
global fpu_restore

section .text

fpu_save:
    test dil, 0x0F
    jz .save_aligned
    push rbp
    mov rbp, rsp
    sub rsp, 512
    and rsp, -16
    fxsave64 [rsp]
    mov rsi, rsp
    mov rcx, 64
    cld
    rep movsq
    mov rsp, rbp
    pop rbp
    ret
.save_aligned:
    fxsave64 [rdi]
    ret

fpu_restore:
    test dil, 0x0F
    jz .restore_aligned
    push rbp
    mov rbp, rsp
    sub rsp, 512
    and rsp, -16
    mov rsi, rdi
    mov rdi, rsp
    mov rcx, 64
    cld
    rep movsq
    fxrstor64 [rsp]
    mov rsp, rbp
    pop rbp
    ret
.restore_aligned:
    fxrstor64 [rdi]
    ret
