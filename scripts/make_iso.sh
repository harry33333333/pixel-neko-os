#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

# 1. Ensure version header exists
if [ ! -f "include/version.h" ]; then
    bash scripts/version_gen.sh
fi

BUILD_ID=$(grep "OS_BUILD" include/version.h | head -1 | cut -d '"' -f 2)
if [ -z "$BUILD_ID" ]; then
    BUILD_ID="26A1001a"
fi

ISO_NAME="pixel-neko-os_${BUILD_ID}.iso"

echo "=== Preparing ISO layout for ${ISO_NAME} ==="
rm -rf iso_root
mkdir -p iso_root/boot/limine iso_root/EFI/BOOT

# Copy kernel
if [ ! -f "bin/kernel.elf" ]; then
    echo "Error: bin/kernel.elf not found! Run make first."
    exit 1
fi
cp bin/kernel.elf iso_root/boot/
cp boot/limine.conf iso_root/boot/limine/

# Copy Limine BIOS & UEFI CD boot images
cp boot/limine/limine-bios.sys iso_root/boot/limine/
cp boot/limine/limine-bios-cd.bin iso_root/boot/limine/
cp boot/limine/limine-uefi-cd.bin iso_root/boot/limine/
cp boot/limine/BOOTX64.EFI iso_root/EFI/BOOT/BOOTX64.EFI

# 2. Build host limine utility if needed
if [ ! -f "boot/limine/limine" ]; then
    if [ -f "boot/limine/limine.c" ]; then
        echo "Compiling host limine installer..."
        gcc -O2 -o boot/limine/limine boot/limine/limine.c
    elif command -v limine >/dev/null 2>&1; then
        cp "$(command -v limine)" boot/limine/limine
    fi
fi

# 3. Create El Torito Dual-Boot Hybrid ISO via xorriso
echo "Building Hybrid ISO with xorriso..."
xorriso -as mkisofs -R -r -J \
        -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table \
        --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso_root -o "${ISO_NAME}"

# 4. Inject Limine hybrid MBR for Legacy BIOS booting from USB/HDD
if [ -f "boot/limine/limine" ]; then
    echo "Installing Limine BIOS boot sector..."
    ./boot/limine/limine bios-install "${ISO_NAME}"
fi

# Create symlink or copy to pixel-neko-os_latest.iso
cp "${ISO_NAME}" pixel-neko-os_latest.iso

echo "=== ISO successfully generated: ${ISO_NAME} ==="
ls -lh "${ISO_NAME}"
