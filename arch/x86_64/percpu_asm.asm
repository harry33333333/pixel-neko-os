; Pixel Neko OS - Shared NMI test flag for SYSCALL/SYSRET paranoid NMI test

[bits 64]
default rel

global per_cpu_nmi_test_pending

section .bss
align 8
per_cpu_nmi_test_pending: resq 1
