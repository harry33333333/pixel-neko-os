# Pixel Neko Storage Stack

## Layers

Pixel Neko OS exposes storage through a 64-bit `block_device_t` abstraction.
AHCI, IDE/ATAPI, ISA floppy and USB Bulk-Only Transport drivers register
devices in the same table and use the same read/write contract.

The mount and drive layer contains:

- MBR primary/logical and GPT partition scanners.
- FAT12/16/32 with long file names and writable file allocation.
- Native exFAT boot-region, FAT-chain, allocation-bitmap, up-case metadata,
  stream-extension and filename-entry parsing.
- ISO9660/Joliet for installation media.
- FAT16 and FAT32 formatting with VBR/BPB, mirrored FATs, FAT32 FSInfo and a
  reserved, initialized root cluster.

## Drive Letters

| Letter | Assignment |
| --- | --- |
| A:, B: | FAT12 floppy media in physical drives 0 and 1 |
| C: | Preferred local FAT system partition |
| D: | Installation/optical media |
| E: | Removable exFAT USB volume |
| F: onward | Additional local or removable volumes |

## Safety & Initialization Policy

To safeguard user data against accidental data loss, raw unpartitioned or
uninitialized disks are strictly prohibited from being silently formatted or
partitioned by the kernel. When an uninitialized storage device is detected:

- The drive is flagged in the kernel storage table (`g_uninit_disk_dev`).
- Auto-partitioning routines are bypassed.
- An interactive modal confirmation dialog is presented on the desktop window
  manager, prompting the user for explicit confirmation before any destructive
  partition table creation or formatting takes place.

## Directory Traversal & VFS Navigation

Path resolution supports full multi-level navigation across FAT12/16/32, exFAT
and ISO9660:
- Folder double-click navigation in File Manager properly enters subdirectories
  without remaining at the parent level.
- Up-directory traversal correctly parses canonical directory parent entries
  (`..`) back to root or drive roots.

## Controllers

AHCI uses a command list, received-FIS area and 64-bit PRDT DMA. The driver
preserves firmware-established SATA links and performs COMRESET only when a
link is not ready. Legacy IDE provides PIO ATA and packet ATAPI fallback.

The floppy stack uses 8237 ISA DMA channel 2 with an address/count buffer
below 16 MiB that never crosses a 64 KiB boundary. The FDC driver probes the
standard MFM data rates and derives the actual capacity, cylinder, head and
sector geometry from the FAT12 BPB. A: and B: are tied to physical units 0
and 1; an empty drive does not create a VFS drive letter. Disk-change
detection invalidates the previous FAT volume before remounting replacement
media.

USB Mass Storage implements Bulk-Only Transport with SCSI INQUIRY, READ
CAPACITY, READ(10), WRITE(10), REQUEST SENSE handling and reset recovery.
UHCI, OHCI, EHCI and xHCI each provide bulk endpoint transport.

## Validation

The P5 QEMU scenario attaches:

- An AHCI optical drive containing the live ISO.
- A FAT32 data disk requiring read and write.
- FAT16 and FAT32 disks that are reformatted by the kernel.
- A writable exFAT USB disk attached through xHCI.

The job requires `FAT read/write PASS`, `exFAT read/write PASS`, both format
passes and the aggregate `Storage self-test PASS` marker.
