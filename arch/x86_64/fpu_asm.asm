; Pixel Neko OS - FPU/SSE/AVX state primitives (XSAVE64 with FXSAVE64 fallback)

[bits 64]
default rel

global fpu_save
global fpu_restore

extern g_fpu_use_xsave
extern g_fpu_xsave_mask

section .text

fpu_save:
    cmp dword [rel g_fpu_use_xsave], 0
    je .use_fxsave
    ; XSAVE64 requires 64-byte alignment
    test dil, 0x3F
    jnz .use_fxsave
    mov rax, [rel g_fpu_xsave_mask]
    mov rdx, rax
    shr rdx, 32
    xsave64 [rdi]
    ret

.use_fxsave:
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
    cmp dword [rel g_fpu_use_xsave], 0
    je .use_fxrstor
    test dil, 0x3F
    jnz .use_fxrstor
    mov rax, [rel g_fpu_xsave_mask]
    mov rdx, rax
    shr rdx, 32
    xrstor64 [rdi]
    ret

.use_fxrstor:
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
