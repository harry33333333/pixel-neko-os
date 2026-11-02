; Pixel Neko OS - Per-CPU data used by SYSCALL and Paranoid NMI paths

[bits 64]
default rel

global per_cpu_block
global per_cpu_kernel_rsp0
global per_cpu_user_rsp
global per_cpu_need_resched
global per_cpu_switch_gs
global per_cpu_current_thread
global per_cpu_nmi_swapgs
global per_cpu_syscall_count
global per_cpu_nmi_count

section .bss
align 64
per_cpu_block:
per_cpu_kernel_rsp0:    resq 1
per_cpu_user_rsp:       resq 1
per_cpu_need_resched:   resq 1
per_cpu_switch_gs:      resq 1
per_cpu_current_thread: resq 1
per_cpu_nmi_swapgs:     resq 1
per_cpu_syscall_count:  resq 1
per_cpu_nmi_count:      resq 1
