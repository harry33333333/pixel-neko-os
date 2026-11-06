#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

ISO="${1:-}"
if [ -z "${ISO}" ] || [ ! -f "${ISO}" ]; then
    echo "Usage: $0 <versioned-iso>" >&2
    exit 2
fi

WORK_DIR=".floppy-test"
mkdir -p "${WORK_DIR}"
truncate -s 1474560 "${WORK_DIR}/a-1440.img"
truncate -s 737280 "${WORK_DIR}/a-720.img"
truncate -s 737280 "${WORK_DIR}/b-720.img"
mformat -i "${WORK_DIR}/a-1440.img" -f 1440 ::
mformat -i "${WORK_DIR}/a-720.img" -f 720 ::
mformat -i "${WORK_DIR}/b-720.img" -f 720 ::

EMPTY_LOG="qemu-floppy-empty-serial.log"
EMPTY_CONSOLE="qemu-floppy-empty-console.log"
rm -f "${EMPTY_LOG}" "${EMPTY_CONSOLE}"
set +e
timeout --foreground 35s qemu-system-x86_64 \
    -machine pc,accel=tcg \
    -cpu max \
    -smp 1 \
    -m 512M \
    -display none \
    -monitor none \
    -serial "file:${EMPTY_LOG}" \
    -cdrom "${ISO}" \
    -boot order=d >"${EMPTY_CONSOLE}" 2>&1
empty_status=$?
set -e
if [ "${empty_status}" -ne 0 ] && [ "${empty_status}" -ne 124 ]; then
    echo "No-floppy QEMU run exited with status ${empty_status}" >&2
    cat "${EMPTY_CONSOLE}" >&2 || true
    exit 1
fi
if ! grep -q "\[P6 \] Full desktop" "${EMPTY_LOG}"; then
    echo "No-floppy run did not reach the desktop" >&2
    tail -n 300 "${EMPTY_LOG}" >&2 || true
    exit 1
fi
if grep -qE "\[VFS \] [AB]:" "${EMPTY_LOG}"; then
    echo "VFS A:/B: appeared without floppy media" >&2
    tail -n 350 "${EMPTY_LOG}" >&2 || true
    exit 1
fi

SERIAL_LOG="qemu-floppy-serial.log"
CONSOLE_LOG="qemu-floppy-console.log"
QMP_SOCKET="qemu-floppy.qmp"
rm -f "${SERIAL_LOG}" "${CONSOLE_LOG}" "${QMP_SOCKET}"
timeout --foreground 55s qemu-system-x86_64 \
    -machine pc,accel=tcg \
    -cpu max \
    -smp 1 \
    -m 512M \
    -display none \
    -monitor none \
    -qmp "unix:${QMP_SOCKET},server=on,wait=off" \
    -serial "file:${SERIAL_LOG}" \
    -drive "if=floppy,index=0,id=fda,format=raw,file=${WORK_DIR}/a-1440.img" \
    -drive "if=floppy,index=1,id=fdb,format=raw,file=${WORK_DIR}/b-720.img" \
    -cdrom "${ISO}" \
    -boot order=d >"${CONSOLE_LOG}" 2>&1 &
qemu_pid=$!

if ! python3 scripts/qmp_floppy_test.py "${QMP_SOCKET}" "${SERIAL_LOG}" \
    "${WORK_DIR}/a-720.img"; then
    kill "${qemu_pid}" 2>/dev/null || true
    wait "${qemu_pid}" 2>/dev/null || true
    tail -n 350 "${SERIAL_LOG}" >&2 || true
    exit 1
fi

kill "${qemu_pid}" 2>/dev/null || true
set +e
wait "${qemu_pid}"
set -e

if ! grep -q "\[FDC \] A: 1440KiB" "${SERIAL_LOG}" ||
   ! grep -q "\[FDC \] B: 720KiB" "${SERIAL_LOG}" ||
   ! grep -q "\[VFS \] A: floppy changed (720 KiB)" "${SERIAL_LOG}" ||
   ! grep -q "\[VFS \] A: floppy media removed" "${SERIAL_LOG}" ||
   ! grep -q "\[VFS \] A: floppy inserted (720 KiB)" "${SERIAL_LOG}"; then
    echo "Floppy capacity or media-change validation failed" >&2
    tail -n 350 "${SERIAL_LOG}" >&2 || true
    exit 1
fi

echo "ISA DMA/FDC A:/B: capacity and media-change test PASS"
