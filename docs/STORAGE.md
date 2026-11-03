# Pixel Neko Storage Stack

## Layers

Pixel Neko OS exposes storage through a 64-bit `block_device_t` abstraction.
AHCI, IDE/ATAPI and USB Bulk-Only Transport drivers register devices in the
same table and use the same read/write contract.

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
| A:, B: | Floppy drives |
| C: | Preferred local FAT system partition |
| D: | Installation/optical media |
| E: | Removable exFAT USB volume |
| F: onward | Additional local or removable volumes |

## Controllers

AHCI uses a command list, received-FIS area and 64-bit PRDT DMA. The driver
preserves firmware-established SATA links and performs COMRESET only when a
link is not ready. Legacy IDE provides PIO ATA and packet ATAPI fallback.

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
