; Pixel Neko OS - SMP & NMI-safe SYSCALL/SYSRET and trap frame restore path

[bits 64]
default rel

global syscall_entry
global arch_enter_thread
global arch_restore_trap_frame

extern syscall_dispatcher
extern syscall_reschedule
extern syscall_bad_return
extern lapic_send_nmi_self
extern per_cpu_nmi_test_pending

section .text

; Per-CPU offsets via %gs (matches include/percpu.h):
;   [gs:0]  = kernel_rsp0
;   [gs:8]  = user_rsp
;   [gs:16] = need_resched
;   [gs:24] = switch_gs
;   [gs:32] = current_thread
;   [gs:40] = nmi_swapgs
;   [gs:48] = syscall_count
;   [gs:56] = nmi_count

; On entry from Ring 3:
;   RDI, RSI, RDX, R10, R8, R9 carry syscall arguments.
;   RCX carries user RIP and R11 carries user RFLAGS.
;   IA32_FMASK has already cleared IF and DF.
syscall_entry:
    swapgs                          ; Enter per-CPU kernel GS domain
    mov [gs:8], rsp                 ; Save user RSP
    mov rsp, [gs:0]                 ; Switch to current thread's kernel stack
    inc qword [gs:48]               ; per_cpu.syscall_count++

    ; Build a full trap_frame_t so the same restore path handles context switches
    push qword 0x1B                 ; user SS (RPL 3)
    push qword [gs:8]               ; user RSP
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

    mov rdi, rsp
    cld
    call syscall_dispatcher

    ; frame->rax is at offset 14 * 8.
    mov [rsp + 14*8], rax
    cmp qword [gs:16], 0            ; per_cpu.need_resched
    jne .reschedule

    ; Reject non-canonical or kernel-half RIP before using SYSRET.
    mov r10, [rsp + 17*8]           ; saved RIP
    test r10, r10
    jz .bad_return
    mov rax, r10
    shr rax, 47
    jnz .bad_return
    mov rax, 0x0000800000000000
    cmp r10, rax
    jae .bad_return

    ; Reject non-canonical or kernel-half RSP before using SYSRET.
    mov r10, [rsp + 20*8]           ; saved RSP
    test r10, r10
    jz .bad_return
    mov rax, r10
    shr rax, 47
    jnz .bad_return
    mov rax, 0x0000800000000000
    cmp r10, rax
    jae .bad_return

    ; Verify CS and SS segments: both must belong to user mode (RPL 3).
    test byte [rsp + 18*8], 3       ; CS RPL must be 3
    jz .bad_return
    test byte [rsp + 21*8], 3       ; SS RPL must be 3
    jz .bad_return

    ; Verify RFLAGS: reject disallowed bits (IOPL != 0, NT != 0, VM != 0).
    mov r11, [rsp + 19*8]           ; saved RFLAGS
    test r11, 0x7000                ; IOPL (0x3000) or NT (0x4000)
    jnz .bad_return
    test r11, 0x20000               ; VM (bit 17)
    jnz .bad_return

    ; Enforce user RFLAGS mask: IOPL=0, NT=0, IF=1, reserved bit 1 = 1.
    and r11, 0x0CD5                 ; Keep arithmetic & direction flags only
    or  r11, 0x0202                 ; Enforce IF=1 and bit 1 = 1
    mov [rsp + 19*8], r11

    ; Restore user DS/ES without touching FS (preserves MSR_FS_BASE TLS!)
    mov ax, 0x1B
    mov ds, ax
    mov es, ax

    ; Restore user GS domain right before SYSRET
    swapgs

    ; Exercise the narrow kernel-CPL/user-GS window before SYSRET.
    cmp qword [rel per_cpu_nmi_test_pending], 0
    je .nmi_test_done
    mov qword [rel per_cpu_nmi_test_pending], 0
    call lapic_send_nmi_self
    mov ecx, 4096
.nmi_test_delay:
    pause
    loop .nmi_test_delay
.nmi_test_done:

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

; Enter an initial or saved trap frame from kernel mode (GS_BASE = kernel per_cpu).
arch_enter_thread:
    mov rsp, rdi
    jmp arch_restore_trap_frame

; Restore a trap_frame_t and return with IRETQ.
; Called from kernel mode (GS_BASE = kernel per_cpu).
; Symmetrically executes SWAPGS when returning to Ring 3.
arch_restore_trap_frame:
    mov qword [gs:24], 0            ; per_cpu.switch_gs = 0

    ; Release previous thread's running_cpu now that RSP is on the next thread's stack
    mov rax, [gs:160]
    test rax, rax
    jz .no_prev_release
    mov qword [gs:160], 0
    mov dword [rax], -1
.no_prev_release:

    test byte [rsp + 18*8], 3       ; frame->cs
    jz .kernel_segments

    ; Returning to user mode: sanitize RFLAGS (IOPL=0, IF=1)
    and dword [rsp + 19*8], 0x0CD5
    or  dword [rsp + 19*8], 0x0202
    mov dword [rsp + 19*8 + 4], 0
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    swapgs                          ; Restore user GS domain
    jmp .pop_regs

.kernel_segments:
    mov ax, 0x10
    mov ds, ax
    mov es, ax

.pop_regs:
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
