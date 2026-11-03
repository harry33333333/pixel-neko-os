#!/usr/bin/env bash
set -euo pipefail

IMAGE_DIR="${1:-.p5-images}"
mkdir -p "${IMAGE_DIR}"

create_fat_image() {
    local image="$1"
    local size_mb="$2"
    local fat_bits="$3"
    local label="$4"
    truncate -s "${size_mb}M" "${image}"
    sfdisk --quiet --label dos "${image}" <<EOF
start=2048, type=0c, bootable
EOF
    mkfs.fat "-F${fat_bits}" -n "${label}" --offset=2048 "${image}"
}

copy_fat_test_file() {
    local image="$1"
    local source="$2"
    local destination="$3"
    mcopy -o -i "${image}@@$((2048 * 512))" "${source}" "::${destination}"
}

create_fat_image "${IMAGE_DIR}/p5-data.img" 512 32 P5DATA
printf 'P5FAT-OK\n' > "${IMAGE_DIR}/p5fat.txt"
copy_fat_test_file "${IMAGE_DIR}/p5-data.img" \
    "${IMAGE_DIR}/p5fat.txt" P5FAT.TXT

create_fat_image "${IMAGE_DIR}/p5-format16.img" 64 16 P5FMT16
create_fat_image "${IMAGE_DIR}/p5-format32.img" 512 32 P5FMT32

truncate -s 128M "${IMAGE_DIR}/p5-exfat.img"
mkfs.exfat -n P5USB "${IMAGE_DIR}/p5-exfat.img"
mount_dir="$(mktemp -d)"
cleanup() {
    sudo umount "${mount_dir}" 2>/dev/null || true
    rmdir "${mount_dir}" 2>/dev/null || true
}
trap cleanup EXIT
sudo mount -o loop "${IMAGE_DIR}/p5-exfat.img" "${mount_dir}"
printf 'P5EXFAT-OK\n' | sudo tee "${mount_dir}/P5EXFAT.TXT" >/dev/null
printf 'P5-EXWRITE-OLD' | sudo tee "${mount_dir}/P5EXWR.TXT" >/dev/null
sudo sync
sudo umount "${mount_dir}"
rmdir "${mount_dir}"
trap - EXIT

ls -lh "${IMAGE_DIR}"
