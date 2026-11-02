; Pixel Neko OS - FPU/SSE state primitives

[bits 64]
default rel

global fpu_save
global fpu_restore

section .text

fpu_save:
    fxsave64 [rdi]
    ret

fpu_restore:
    fxrstor64 [rdi]
    ret
