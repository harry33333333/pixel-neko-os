; Pixel Neko OS - Ring 3 PXE C Runtime Entry (crt0_r3)
[bits 64]
default rel

extern main
extern pxe_exit

global _start

section .text
_start:
    xor ebp, ebp
    mov edi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv
    and rsp, -16            ; align stack per System V AMD64 ABI
    call main
    mov edi, eax
    call pxe_exit
.hang:
    pause
    jmp .hang
