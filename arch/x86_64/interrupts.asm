; Pixel Neko OS - 64-Bit Interrupt, Exception & SMP IPI Stubs
; Architecture: AMD64 / x86-64 Long Mode

[bits 64]
default rel

extern c_exception_handler
extern c_irq_handler
extern c_kernel_yield_handler
extern c_ipi_resched_handler
extern c_ipi_tlb_handler
extern c_ipi_panic_handler
extern arch_restore_trap_frame

%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push 0                  ; Push dummy error code
    push %1                 ; Push interrupt number
    jmp common_isr_stub
%endmacro

%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push %1                 ; CPU already pushed error code, push interrupt number
    jmp common_isr_stub
%endmacro

%macro IRQ 2
global irq%1
irq%1:
    push 0                  ; Dummy error code
    push %2                 ; Interrupt vector number
    jmp common_irq_stub
%endmacro

%macro PUSH_GPRS 0
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
%endmacro

section .text

; 0..31 CPU Exceptions
ISR_NOERRCODE 0   ; #DE Divide-by-zero
ISR_NOERRCODE 1   ; #DB Debug
ISR_NOERRCODE 2   ; NMI
ISR_NOERRCODE 3   ; #BP Breakpoint
ISR_NOERRCODE 4   ; #OF Overflow
ISR_NOERRCODE 5   ; #BR Bound Range Exceeded
ISR_NOERRCODE 6   ; #UD Invalid Opcode
ISR_NOERRCODE 7   ; #NM Device Not Available
ISR_ERRCODE   8   ; #DF Double Fault (uses IST1!)
ISR_NOERRCODE 9   ; Coprocessor Segment Overrun
ISR_ERRCODE   10  ; #TS Invalid TSS
ISR_ERRCODE   11  ; #NP Segment Not Present
ISR_ERRCODE   12  ; #SS Stack-Segment Fault
ISR_ERRCODE   13  ; #GP General Protection Fault
ISR_ERRCODE   14  ; #PF Page Fault
ISR_NOERRCODE 15  ; Reserved
ISR_NOERRCODE 16  ; #MF x87 FPU Floating-Point Error
ISR_ERRCODE   17  ; #AC Alignment Check
ISR_NOERRCODE 18  ; #MC Machine Check
ISR_NOERRCODE 19  ; #XM SIMD Floating-Point Exception
ISR_NOERRCODE 20  ; #VE Virtualization Exception
ISR_ERRCODE   21  ; #CP Control Protection Exception
ISR_NOERRCODE 22  ; Reserved
ISR_NOERRCODE 23  ; Reserved
ISR_NOERRCODE 24  ; Reserved
ISR_NOERRCODE 25  ; Reserved
ISR_NOERRCODE 26  ; Reserved
ISR_NOERRCODE 27  ; Reserved
ISR_NOERRCODE 28  ; #HV Hypervisor Injection Exception
ISR_ERRCODE   29  ; #VC VMM Communication Exception
ISR_ERRCODE   30  ; #SX Security Exception
ISR_NOERRCODE 31  ; Reserved

; 32..55 Hardware IRQs (APIC IOAPIC + LAPIC Timer)
IRQ 0, 32   ; Timer
IRQ 1, 33   ; PS/2 Keyboard
IRQ 2, 34   ; Cascade
IRQ 3, 35   ; COM2
IRQ 4, 36   ; COM1
IRQ 5, 37   ; LPT2
IRQ 6, 38   ; Floppy
IRQ 7, 39   ; LPT1 / Spurious
IRQ 8, 40   ; CMOS RTC
IRQ 9, 41   ; Free
IRQ 10, 42  ; Free
IRQ 11, 43  ; Free
IRQ 12, 44  ; PS/2 Mouse
IRQ 13, 45  ; FPU Coprocessor
IRQ 14, 46  ; Primary ATA
IRQ 15, 47  ; Secondary ATA
IRQ 16, 48  ; IOAPIC GSI 16 / PCI INTx
IRQ 17, 49  ; IOAPIC GSI 17 / PCI INTx
IRQ 18, 50  ; IOAPIC GSI 18 / PCI INTx
IRQ 19, 51  ; IOAPIC GSI 19 / PCI INTx
IRQ 20, 52  ; IOAPIC GSI 20 / PCI INTx
IRQ 21, 53  ; IOAPIC GSI 21 / PCI INTx
IRQ 22, 54  ; IOAPIC GSI 22 / PCI INTx
IRQ 23, 55  ; IOAPIC GSI 23 / PCI INTx

; Common Exception Handler Stub
common_isr_stub:
    PUSH_GPRS
    test byte [rsp + 18*8], 3
    jz .from_kernel
    swapgs
.from_kernel:
    mov rdi, rsp
    cld
    call c_exception_handler
    mov rsp, rax
    jmp arch_restore_trap_frame

; Software Interrupt 0x81: Cooperative Kernel Thread Yield / Sleep / Exit (DPL 0)
global isr_kernel_yield
isr_kernel_yield:
    push 0
    push 0x81
    PUSH_GPRS
    mov rdi, rsp
    cld
    call c_kernel_yield_handler
    mov rsp, rax
    jmp arch_restore_trap_frame

; Common IRQ Handler Stub
common_irq_stub:
    PUSH_GPRS
    test byte [rsp + 18*8], 3
    jz .from_kernel
    swapgs
.from_kernel:
    mov rdi, rsp
    cld
    call c_irq_handler
    mov rsp, rax
    jmp arch_restore_trap_frame

; SMP IPI 0xF0: Reschedule
global isr_ipi_resched
isr_ipi_resched:
    push 0
    push 0xF0
    PUSH_GPRS
    test byte [rsp + 18*8], 3
    jz .from_kernel
    swapgs
.from_kernel:
    mov rdi, rsp
    cld
    call c_ipi_resched_handler
    mov rsp, rax
    jmp arch_restore_trap_frame

; SMP IPI 0xF1: TLB Shootdown
global isr_ipi_tlb
isr_ipi_tlb:
    push 0
    push 0xF1
    PUSH_GPRS
    test byte [rsp + 18*8], 3
    jz .from_kernel
    swapgs
.from_kernel:
    mov rdi, rsp
    cld
    call c_ipi_tlb_handler
    mov rsp, rax
    jmp arch_restore_trap_frame

; SMP IPI 0xF2: Panic Halt (freeze AP cleanly)
global isr_ipi_panic
isr_ipi_panic:
    cli
    call c_ipi_panic_handler
.halt_loop:
    hlt
    jmp .halt_loop

; Spurious interrupt handler (no EOI needed, just IRETQ)
global isr_spurious
isr_spurious:
    iretq
