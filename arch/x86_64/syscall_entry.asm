; Pixel Neko OS - NMI-safe SYSCALL/SYSRET and transition restore path

[bits 64]
default rel

global syscall_entry
global arch_enter_thread
global arch_restore_trap_frame

extern syscall_dispatcher
extern syscall_reschedule
extern syscall_bad_return
extern per_cpu_set_gs_domain
extern per_cpu_kernel_rsp0
extern per_cpu_user_rsp
extern per_cpu_need_resched
extern per_cpu_switch_gs
extern per_cpu_syscall_count

section .text

; On entry:
;   RDI, RSI, RDX, R10, R8, R9 carry syscall arguments.
;   RCX carries user RIP and R11 carries user RFLAGS.
; IA32_FMASK has already cleared IF and DF.
syscall_entry:
    mov [rel per_cpu_user_rsp], rsp
    mov rsp, [rel per_cpu_kernel_rsp0]
    inc qword [rel per_cpu_syscall_count]

    ; Build a full trap_frame_t. This permits the same restore path to be
    ; used after an IRQ/syscall-triggered context switch.
    push qword 0x1B                 ; user SS (RPL 3)
    push qword [rel per_cpu_user_rsp]
    push r11                        ; user RFLAGS
    push qword 0x23                 ; user CS (RPL 3)
    push rcx                        ; user RIP
    push qword 0                    ; error code
    push qword 0x100                ; syscall frame marker

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

    xor edi, edi
    call per_cpu_set_gs_domain       ; deterministically enter kernel GS domain

    mov rdi, rsp
    cld
    call syscall_dispatcher

    ; frame->rax is at offset 14 * 8.
    mov [rsp + 14*8], rax
    cmp qword [rel per_cpu_need_resched], 0
    jne .reschedule

    ; Reject non-canonical/user-half RIPs before using SYSRET.
    mov r10, [rsp + 17*8]           ; saved RIP
    mov rax, r10
    shr rax, 47
    jnz .bad_return
    mov rax, 0x0000800000000000
    cmp r10, rax
    jae .bad_return

    ; RFLAGS.IOPL must remain zero. IF is forced on for userspace.
    mov r11, [rsp + 19*8]           ; saved RFLAGS
    and r11, 0x3CD5
    or  r11, 0x0202
    mov [rsp + 19*8], r11

    ; Data selectors are loaded before restoring user registers.
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov edi, 1
    call per_cpu_set_gs_domain       ; restore the Ring 3 GS domain

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

    add rsp, 16                     ; skip marker and error code
    mov rcx, [rsp]
    mov r11, [rsp + 16]
    mov rsp, [rsp + 24]
    o64 sysret

.reschedule:
    mov rdi, rsp
    call syscall_reschedule
    mov rsp, rax
    jmp arch_restore_trap_frame

.bad_return:
    mov rdi, rsp
    call syscall_bad_return
    mov rsp, rax
    jmp arch_restore_trap_frame

; Enter an initial or saved trap frame. The scheduler has already selected
; the address space and set per_cpu_switch_gs when a GS-domain change is due.
arch_enter_thread:
    mov rsp, rdi
    jmp arch_restore_trap_frame

; Restore a trap_frame_t and return with IRETQ. This also handles an IST stack
; switch and same-CPL kernel frames correctly.
arch_restore_trap_frame:
    mov qword [rel per_cpu_switch_gs], 0

    test byte [rsp + 18*8], 3       ; frame->cs
    jz .kernel_segments
    mov ax, 0x1B
    jmp .load_segments
.kernel_segments:
    mov ax, 0x10
.load_segments:
    mov ds, ax
    mov es, ax
    mov fs, ax

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
