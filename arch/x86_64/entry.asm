; Pixel Neko OS - 64-Bit Bootstrap Entry
; Target: AMD64 / x86-64 Long Mode (Handover from Limine Bootloader)

[bits 64]
default rel

global kentry
global initial_stack_bottom
global initial_stack_top
extern kmain

section .bss
align 16
initial_stack_bottom:
    resb 32768      ; 32 KB initial bootstrap kernel stack
initial_stack_top:

section .text
kentry:
    ; 1. Disable interrupts and clear string direction flag
    cli
    cld

    ; 2. Ensure stack pointer is 16-byte aligned.
    ; Limine guarantees RSP points to a valid 64-bit stack.
    ; If RSP is null (should never happen), fall back to internal bootstrap stack.
    test rsp, rsp
    jnz .stack_ok
    lea rsp, [initial_stack_top]
.stack_ok:
    and rsp, -16

    ; 3. Reset frame pointer
    xor rbp, rbp

    ; 4. Call C-level kernel entry point
    call kmain

    ; 5. Halt if kmain ever returns
.halt:
    cli
    hlt
    jmp .halt
