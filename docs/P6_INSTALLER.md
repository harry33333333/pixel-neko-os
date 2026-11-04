# P6 Desktop and Offline Installer

Build version: `26A176a`

## Boot modes

The ISO exposes the following Limine entries:

1. Live Desktop
2. Graphical Installer
3. Safe Graphics Live
4. Safe Graphics Installer

`mode=install` opens the installer in a near-full-screen classic window after
the desktop and splash have initialized.

## Installer flow

The installer operates only on an existing writable AHCI/IDE GPT or MBR
partition. It never creates, resizes or deletes partitions.

1. Select an existing partition.
2. Choose FAT32 (default) or FAT16 when the partition size is compatible.
3. Confirm the destructive format of that partition.
4. Format its VBR/BPB, FSInfo, FAT mirrors and root directory.
5. Recursively copy the complete offline payload.
6. Verify the kernel, EFI loader, Limine BIOS stage and configuration.
7. Deploy Limine UEFI and BIOS boot files.

Supported end states:

- UEFI boots `EFI/BOOT/BOOTX64.EFI` from the installed FAT volume.
- BIOS boots the embedded Limine Stage 1/2 from GPT or MBR and loads
  `boot/limine/limine-bios.sys`.
- `boot/limine.conf`, `boot/limine/limine.conf` and `limine.conf` are written
  for bootloader discovery compatibility.

GPT embedding follows Limine's standard algorithm: unused partition-entry
slots are compressed, the primary and backup GPT CRCs are recalculated, and
the Stage 2 halves are placed in the resulting reserved areas. Existing
partition start/end sectors and type GUIDs are preserved.

## Payload

`payload/` on the ISO contains:

- `boot/kernel.elf`
- `boot/limine.conf`
- `boot/limine/limine.conf`
- `boot/limine/limine-bios.sys`
- `EFI/BOOT/BOOTX64.EFI`
- `system/LICENSE.TXT`
- `system/PAYLOAD.TXT`
- `system/README.TXT`
- `system/THIRDPARTY.TXT`

The desktop, nine applications, fonts, wallpaper assets and splash resources
are statically linked into `kernel.elf`, as required by the P6 stage-one
implementation.

## Automated acceptance

The P6 CI matrix installs through the actual graphical installer and then
boots the installed disk without an ISO attached:

- Live desktop
- Installer into an existing GPT partition
- Installer into an existing MBR partition
- Installer into an existing MBR partition using FAT16
- Installed GPT disk through UEFI
- Installed GPT disk through Legacy BIOS
- Installed MBR disk through Legacy BIOS
- Installed FAT16 MBR disk through Legacy BIOS

The tests validate the rendered installer, completion dialog, copied payload
size, on-disk `limine-bios.sys` identity and full desktop boot from the
installed disk.
