; Pixel Neko OS - Ring 3 ELF entry point

[bits 64]
default rel

global _start
extern user_main
extern neko_exit

section .text
_start:
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    and rsp, -16
    call user_main

    mov rdi, rax
    call neko_exit

.hang:
    pause
    jmp .hang
