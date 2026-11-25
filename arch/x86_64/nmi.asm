; Pixel Neko OS - SMP-safe Paranoid NMI path (IST2)
;
; IDT vector 2 uses IST2. That guarantees a clean per-CPU kernel stack even
; if an NMI lands in the narrow SYSCALL prologue or SYSRET epilogue.

[bits 64]
default rel

global nmi_entry

extern c_nmi_handler

section .text
nmi_entry:
    push qword 0                    ; error code
    push qword 2                    ; vector (NMI)
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Check if MSR_GS_BASE currently holds a higher-half kernel per_cpu pointer
    ; (bit 63 == 1). If not (bit 63 == 0), we interrupted user mode or the
    ; narrow SYSCALL/SYSRET user-GS window, so we must SWAPGS.
    mov ecx, 0xC0000101             ; MSR_GS_BASE
    rdmsr
    test edx, 0x80000000
    jnz .gs_already_kernel

    swapgs
    mov qword [gs:40], 1            ; per_cpu.nmi_swapgs = 1
    jmp .gs_ready

.gs_already_kernel:
    mov qword [gs:40], 0            ; per_cpu.nmi_swapgs = 0

.gs_ready:
    mov rdi, rsp
    cld
    call c_nmi_handler

    cmp qword [gs:40], 0
    je .restore
    swapgs

.restore:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq
