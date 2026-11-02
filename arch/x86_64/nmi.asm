; Pixel Neko OS - Paranoid NMI path
;
; IDT vector 2 uses IST2. That guarantees a kernel stack even if NMI lands in
; the narrow SYSCALL prologue after SWAPGS but before switching RSP, or in the
; SYSRET epilogue after SWAPGS but before the privilege return.

[bits 64]
default rel

global nmi_entry

extern c_nmi_handler
extern per_cpu_block
extern per_cpu_nmi_swapgs

section .text
nmi_entry:
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
    push qword 0                    ; error code
    push qword 2                    ; vector

    mov qword [rel per_cpu_nmi_swapgs], 0

    ; CS is at offset 18 * 8 in the completed trap frame.
    test byte [rsp + 18*8], 3
    jnz .from_user

    ; Ring 0 can still own the user GS base in the SYSCALL prologue/epilogue.
    mov ecx, 0xC0000101             ; MSR_GS_BASE
    rdmsr
    shl rdx, 32
    or rax, rdx
    lea rdx, [rel per_cpu_block]
    cmp rax, rdx
    je .gs_correct

.from_user:
    swapgs
    mov qword [rel per_cpu_nmi_swapgs], 1

.gs_correct:
    mov rdi, rsp
    cld
    call c_nmi_handler

    cmp qword [rel per_cpu_nmi_swapgs], 0
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
