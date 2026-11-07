#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

TEST_KIND="${1:-}"
if [ -n "${TEST_KIND}" ] &&
   [ "${TEST_KIND}" != "acpi-uacpi" ] &&
   [ "${TEST_KIND}" != "acpi-fallback" ]; then
    echo "Unknown ISO test kind: ${TEST_KIND}" >&2
    exit 2
fi

# 1. Ensure version header exists
if [ ! -f "include/version.h" ]; then
    bash scripts/version_gen.sh
fi

BUILD_ID=$(grep "OS_BUILD" include/version.h | head -1 | cut -d '"' -f 2)
if [ -z "$BUILD_ID" ]; then
    BUILD_ID="26A200a"
fi

ISO_NAME="pixel-neko-os_${BUILD_ID}.iso"
if [ -n "${TEST_KIND}" ]; then
    ISO_NAME="pixel-neko-os_${BUILD_ID}-${TEST_KIND}-test.iso"
fi

echo "=== Preparing ISO layout for ${ISO_NAME} ==="
rm -rf iso_root
mkdir -p iso_root/boot/limine iso_root/EFI/BOOT iso_root/PDEVICE
mkdir -p iso_root/payload/boot/limine iso_root/payload/EFI/BOOT iso_root/payload/PDEVICE
mkdir -p iso_root/payload/system

# Copy kernel
if [ ! -f "bin/kernel.elf" ]; then
    echo "Error: bin/kernel.elf not found! Run make first."
    exit 1
fi
cp bin/kernel.elf iso_root/boot/
cp boot/limine.conf iso_root/boot/limine.conf
cp boot/limine.conf iso_root/boot/limine/

# Copy native loadable PDV drivers for Live CD and offline installer payload
shopt -s nullglob
PDV_FILES=(build/pdevice/*.pdv)
shopt -u nullglob
if [ "${#PDV_FILES[@]}" -eq 0 ]; then
    echo "Error: No PDV drivers found in build/pdevice/! Run make first."
    exit 1
fi
cp "${PDV_FILES[@]}" iso_root/PDEVICE/
cp "${PDV_FILES[@]}" iso_root/payload/PDEVICE/

# Copy Limine BIOS & UEFI CD boot images
cp boot/limine/limine-bios.sys iso_root/boot/limine/
cp boot/limine/limine-bios-cd.bin iso_root/boot/limine/
cp boot/limine/limine-uefi-cd.bin iso_root/boot/limine/
cp boot/limine/BOOTX64.EFI iso_root/EFI/BOOT/BOOTX64.EFI

if [ -n "${TEST_KIND}" ]; then
    sed -i \
        's/mode=\(live\|install\)/mode=\1 shutdown_test=1/' \
        iso_root/boot/limine.conf
    if [ "${TEST_KIND}" = "acpi-fallback" ]; then
        sed -i 's/shutdown_test=1/shutdown_test=1 disable_uacpi=1/' \
            iso_root/boot/limine.conf
    fi
fi

# Build the offline install payload.
cp bin/kernel.elf iso_root/payload/boot/kernel.elf
cp boot/limine.conf iso_root/payload/limine.conf
cp boot/limine.conf iso_root/payload/boot/limine.conf
cp boot/limine.conf iso_root/payload/boot/limine/limine.conf
cp boot/limine/limine-bios.sys iso_root/payload/boot/limine/limine-bios.sys
cp boot/limine/BOOTX64.EFI iso_root/payload/EFI/BOOT/BOOTX64.EFI
cp boot/limine-installed.conf iso_root/payload/system/limine-installed.conf
cp LICENSE iso_root/payload/system/LICENSE.TXT
cp THIRD_PARTY_LICENSES.md iso_root/payload/system/THIRDPARTY.TXT
cp README.md iso_root/payload/system/README.TXT
cat > iso_root/payload/system/PAYLOAD.TXT <<EOF
Pixel Neko OS ${BUILD_ID} complete offline payload

Static system executable: boot/kernel.elf
Loadable hardware drivers: PDEVICE/*.pdv
UEFI boot executable: EFI/BOOT/BOOTX64.EFI
BIOS Limine stage 2: boot/limine/limine-bios.sys
Boot configuration: boot/limine.conf
Legacy compatibility configuration: boot/limine/limine.conf
Installed-system configuration: system/limine-installed.conf

Loadable PDV drivers in PDEVICE/:
- Display: vmwgfx.pdv, mach64.pdv, nvg7x.pdv
- Audio: hdaudio.pdv, ac97.pdv, ct4810.pdv, cmi8738.pdv
- Storage: fdc.pdv

Embedded in kernel.elf:
- Core kernel, USB stack, boot storage and filesystems
- Classic window manager, built-in desktop applications and bitmap resources

The installer recursively releases this tree to the selected existing
partition, then deploys Limine UEFI and BIOS boot files. The partition table
is not repartitioned or rewritten.
EOF

# 2. Build host limine utility if needed
if [ ! -f "boot/limine/limine" ]; then
    if [ -f "boot/limine/limine.c" ]; then
        echo "Compiling host limine installer..."
        gcc -O2 -Iboot/limine -o boot/limine/limine boot/limine/limine.c
        chmod +x boot/limine/limine
    elif command -v limine >/dev/null 2>&1; then
        cp "$(command -v limine)" boot/limine/limine
        chmod +x boot/limine/limine
    fi
fi
chmod +x boot/limine/limine 2>/dev/null || true

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
if [ -z "${TEST_KIND}" ]; then
    cp "${ISO_NAME}" pixel-neko-os_latest.iso
fi

echo "=== ISO successfully generated: ${ISO_NAME} ==="
ls -lh "${ISO_NAME}"
