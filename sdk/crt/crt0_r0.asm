; Pixel Neko OS - Ring 0 PXE C Runtime Entry (crt0_r0)
[bits 64]
default rel

extern main
extern pxe_exit
extern __pxe_r0_api

global _start_r0

section .text
_start_r0:
    xor ebp, ebp
    mov [rel __pxe_r0_api], rdx
    and rsp, -16            ; align stack per System V AMD64 ABI (RDI=argc, RSI=argv preserved)
    call main
    mov edi, eax
    call pxe_exit
.hang:
    pause
    jmp .hang
