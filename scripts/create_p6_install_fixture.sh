#!/usr/bin/env bash
set -euo pipefail

IMAGE_DIR="${1:-.p6-images}"
mkdir -p "${IMAGE_DIR}"

IMAGE="${IMAGE_DIR}/p6-target.img"
MbrImage="${IMAGE_DIR}/p6-mbr.img"
Fat16Image="${IMAGE_DIR}/p6-fat16.img"
truncate -s 1G "${IMAGE}"
truncate -s 512M "${MbrImage}"
truncate -s 128M "${Fat16Image}"

sfdisk --quiet --label gpt "${IMAGE}" <<'EOF'
label: gpt
start=2048, size=2095070, type=uefi
EOF

mkfs.fat -F32 -n P6TARGET --offset=2048 "${IMAGE}"

sfdisk --quiet --label dos "${MbrImage}" <<'EOF'
label: dos
start=2048, size=1046527, type=0c, bootable
EOF
mkfs.fat -F32 -n P6MBR --offset=2048 "${MbrImage}"

sfdisk --quiet --label dos "${Fat16Image}" <<'EOF'
label: dos
start=2048, size=260095, type=0e, bootable
EOF
mkfs.fat -F16 -n P6FAT16 --offset=2048 "${Fat16Image}"

MARKER="$(mktemp)"
trap 'rm -f "${MARKER}"' EXIT
printf 'MUST-BE-ERASED-BY-INSTALLER\n' > "${MARKER}"
mcopy -o -i "${IMAGE}@@1048576" "${MARKER}" ::OLD.TXT
mcopy -o -i "${MbrImage}@@1048576" "${MARKER}" ::OLD.TXT
mcopy -o -i "${Fat16Image}@@1048576" "${MARKER}" ::OLD.TXT

sfdisk -d "${IMAGE}"
mdir -i "${IMAGE}@@1048576" ::/
sfdisk -d "${MbrImage}"
mdir -i "${MbrImage}@@1048576" ::/
sfdisk -d "${Fat16Image}"
mdir -i "${Fat16Image}@@1048576" ::/
