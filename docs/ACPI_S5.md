# ACPI S5 Shutdown

Build version: `26A180a`

## Shutdown order

1. `sys_shutdown()` synchronously flushes audio and FAT state.
2. The preferred path evaluates the live AML namespace and `_S5_` through
   uACPI, then requests `UACPI_SLEEP_STATE_S5`. uACPI handles FADT GAS
   register access and `_PTS` where firmware provides it.
3. If uACPI cannot initialize or complete S5, a validated AML pattern matcher
   extracts `SLP_TYPa` and `SLP_TYPb` from DSDT/SSDT.
4. The fallback writes the FADT PM1 control registers through their Generic
   Address Structure. System I/O and System Memory GAS spaces are supported,
   including register bit width, bit offset and access width.
5. If no validated `_S5_` package or PM1 control GAS is available, hardware
   registers are left untouched and the classic safe-to-power-off screen is
   shown.

The original blind PM1 `SLP_TYP=7/5/0` guesses and fixed emulator power-off
ports were removed.

## uACPI integration

- Upstream: `uACPI/uACPI`
- Commit: `c0edecef62bcac6ce3f783540099190bafadadda`
- Version: 7.0.0
- License: MIT
- Platform glue: `drivers/acpi/uacpi_glue.c`

The glue supplies RSDP discovery, physical mapping, PCI configuration access,
x86 I/O port access, allocation, time/stall/sleep, semaphore, interrupt-state,
spinlock and deferred-work callbacks.

## Focused test

`scripts/acpi_s5_test.sh` creates two temporary test ISOs from the same kernel:

- `acpi-uacpi`: requires namespace load, `_S5_` package evaluation, namespace
  initialization and the uACPI S5 transition.
- `acpi-fallback`: disables uACPI and requires the AML `_S5_` pattern matcher,
  FADT GAS fallback write and successful QEMU power-off.

Both tests also run an in-kernel MMIO GAS register self-test. The test pass
condition is a clean QEMU exit from ACPI power-off, not merely drawing the
safe shutdown screen.
