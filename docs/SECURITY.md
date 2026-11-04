# Pixel Neko Security Architecture

## Overview

Pixel Neko OS enforces strict architectural boundaries between Ring 0 (Kernel) and Ring 3 (User Space) on the AMD64 (x86-64) architecture, preventing privilege escalation, kernel memory corruption, and non-canonical CPU faults.

## System Call Pipeline (`syscall_entry.asm`)

### 1. Stack & State Isolation
- **Separate Kernel Stack**: Upon `SYSCALL`, the CPU immediately switches from the user stack (`rsp`) to an independent per-CPU kernel stack (`per_cpu_kernel_rsp0`).
- **Complete Register Preservation**: Builds a full `trap_frame_t` containing all general-purpose registers, user RIP, CS, RFLAGS, RSP, and SS.
- **Deterministic GS Domain**: Calls `per_cpu_set_gs_domain(0)` to enter the kernel GS domain safely before dispatching to `syscall_dispatcher`.

### 2. Comprehensive Return Validation (Pre-SYSRET Checks)

Before executing `sysret` to return to Ring 3, the kernel executes strict architectural verifications:

1. **User RIP Validation**:
   - Must be non-NULL (`test r10, r10`).
   - Must be canonical (bits 47..63 all 0 via `shr rax, 47`).
   - Must reside strictly in the user half (`< 0x0000800000000000`).
   - Prevents the Intel SYSRET vulnerability where returning to a non-canonical RIP triggers a kernel `#GP` with user registers.
2. **User RSP Validation**:
   - Must be non-NULL (`test r10, r10`).
   - Must be canonical (bits 47..63 all 0).
   - Must reside strictly in the user half (`< 0x0000800000000000`).
   - Prevents user code from executing on or reading/writing kernel stack addresses.
3. **Segment Privilege (RPL 3)**:
   - User CS and SS selectors must have RPL=3 (`test byte [rsp + offset], 3`).
4. **RFLAGS Sanitization & Restriction**:
   - Disallowed privileged bits are detected and blocked:
     - **IOPL (bits 12-13)**: Must be 0; user mode is never granted direct I/O port permissions.
     - **NT (bit 14)**: Nested Task must be 0 to prevent hardware task-switch faults.
     - **VM (bit 17)**: Virtual-8086 mode is strictly rejected in 64-bit mode.
   - User RFLAGS is sanitized with mask `0x0CD5` (retaining only arithmetic flags CF, PF, AF, ZF, SF, OF, and direction flag DF).
   - Interrupt Flag (IF, bit 9) is enforced ON (`0x0200`) and reserved bit 1 is forced to 1.

### 3. Rogue Return Mitigation (`.bad_return`)

If any validation check fails:
1. The return bypasses `sysret` and routes to `.bad_return`.
2. `syscall_bad_return()` records a detailed diagnostic log containing RIP, RSP, RFLAGS, CS, and SS.
3. The offending user process is terminated immediately (`process_exit_current(-1)`).
4. The scheduler picks the next runnable thread and resumes via `arch_restore_trap_frame`, returning exclusively through the hardware-checked `IRETQ` instruction.

## NMI & Exception Protections

- **Paranoid NMI Handler**: Implements a dedicated NMI IST stack and validates `GS_BASE` identity. The NMI stub guarantees that the transient kernel-CPL/user-GS window before `sysret` does not leak SWAPGS state.
- **Double Fault IST**: Double Faults (`#DF`) are assigned a dedicated 16 KiB IST stack with unmapped guard pages, completely eliminating Triple Fault reboot loops.

