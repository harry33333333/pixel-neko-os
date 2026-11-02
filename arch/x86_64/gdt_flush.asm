; Pixel Neko OS - GDT & TSS Flush Helpers (x86-64)

[bits 64]
default rel

global gdt_flush
global tss_flush

section .text

gdt_flush:
    ; RDI holds pointer to gdt_ptr_t
    lgdt [rdi]

    ; Reload 64-bit kernel data segment registers
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Reload 64-bit CS register via far return (push CS, push RIP, far ret)
    push 0x08
    lea rax, [rel .reload_cs]
    push rax
    db 0x48, 0xCB ; 64-bit far return (REX.W + RETF)
.reload_cs:
    ret

tss_flush:
    ; DI holds TSS selector (0x28)
    ltr di
    ret
