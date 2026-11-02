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

UEFI_FW=""
for candidate in \
    /usr/share/OVMF/OVMF.fd \
    /usr/share/ovmf/OVMF.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF_CODE.fd; do
    if [ -f "${candidate}" ]; then
        UEFI_FW="${candidate}"
        break
    fi
done

if [ -z "${UEFI_FW}" ]; then
    echo "OVMF firmware not found" >&2
    exit 3
fi

run_mode() {
    local mode="$1"
    shift
    local log="qemu-${mode}-serial.log"
    local console="qemu-${mode}-console.log"
    local status=0

    rm -f "${log}" "${console}"
    set +e
    timeout --foreground 22s qemu-system-x86_64 \
        -machine pc,accel=tcg \
        -cpu max \
        -smp 1 \
        -m 512M \
        -display none \
        -monitor none \
        -no-reboot \
        -serial "file:${log}" \
        -cdrom "${ISO}" \
        "$@" >"${console}" 2>&1
    status=$?
    set -e

    if [ "${status}" -ne 0 ] && [ "${status}" -ne 124 ]; then
        echo "QEMU ${mode} mode exited with status ${status}" >&2
        cat "${console}" >&2 || true
        cat "${log}" >&2 || true
        return 1
    fi

    if ! grep -q "SYSTEM.*P3 milestone reached" "${log}" ||
       ! grep -q "Ring 3 processes" "${log}" ||
       ! grep -q "P3 PASS: Ring 3, SYSCALL, FPU context, IPC queue and shared memory" "${log}"; then
        echo "QEMU ${mode} mode did not reach the P3 runtime self-test" >&2
        tail -n 250 "${log}" >&2 || true
        return 1
    fi

    echo "QEMU ${mode} boot and P3 self-test passed."
}

run_mode uefi -bios "${UEFI_FW}"
run_mode bios -boot d
