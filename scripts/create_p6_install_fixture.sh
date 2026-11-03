#!/usr/bin/env bash
set -euo pipefail

IMAGE_DIR="${1:-.p6-images}"
mkdir -p "${IMAGE_DIR}"

IMAGE="${IMAGE_DIR}/p6-target.img"
truncate -s 1G "${IMAGE}"

sfdisk --quiet --label gpt "${IMAGE}" <<'EOF'
label: gpt
start=2048, size=2095070, type=uefi
EOF

mkfs.fat -F32 -n P6TARGET --offset=2048 "${IMAGE}"

MARKER="$(mktemp)"
trap 'rm -f "${MARKER}"' EXIT
printf 'MUST-BE-ERASED-BY-INSTALLER\n' > "${MARKER}"
mcopy -o -i "${IMAGE}@@1048576" "${MARKER}" ::OLD.TXT

sfdisk -d "${IMAGE}"
mdir -i "${IMAGE}@@1048576" ::/
