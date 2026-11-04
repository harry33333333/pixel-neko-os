#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${ROOT_DIR}"

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

VERSION=$(grep "OS_BUILD" include/version.h | head -1 | cut -d '"' -f 2)
if [ -z "${VERSION}" ]; then
    echo "Unable to determine OS build version" >&2
    exit 3
fi

run_s5_test() {
    local kind="$1"
    local iso="pixel-neko-os_${VERSION}-${kind}-test.iso"
    local log="qemu-${kind}-serial.log"
    local console="qemu-${kind}-console.log"
    local status=0

    bash scripts/make_iso.sh "${kind}"
    rm -f "${log}" "${console}"
    set +e
    timeout --foreground 45s qemu-system-x86_64 \
        -machine q35,accel=tcg \
        -cpu max \
        -smp 1 \
        -m 512M \
        -display none \
        -monitor none \
        -no-reboot \
        -serial "file:${log}" \
        -bios "${UEFI_FW}" \
        -device qemu-xhci,id=xhci \
        -device usb-kbd,bus=xhci.0,id=usb-kbd \
        -cdrom "${iso}" \
        >"${console}" 2>&1
    status=$?
    set -e

    if [ "${status}" -ne 0 ]; then
        echo "ACPI S5 ${kind} QEMU exited with status ${status}" >&2
        cat "${console}" >&2 || true
        tail -n 350 "${log}" >&2 || true
        return 1
    fi
    if ! grep -q "\[POWER\] Preparing ACPI S5 shutdown" "${log}" ||
       ! grep -q "\[ACPI\] FADT GAS I/O/MMIO helper self-test: PASS" "${log}" ||
       ! grep -q "\[ACPI TEST\] Starting direct S5 shutdown test" "${log}"; then
        echo "ACPI S5 ${kind} did not reach the shutdown test" >&2
        tail -n 350 "${log}" >&2
        return 1
    fi

    if [ "${kind}" = "acpi-uacpi" ]; then
        if ! grep -q "\[uACPI\] DSDT/SSDT namespace loaded" "${log}" ||
           ! grep -q "\[uACPI\] _S5_ namespace package" "${log}" ||
           ! grep -q "\[uACPI\] Namespace initialized; S5 sleep backend ready" "${log}" ||
           ! grep -q "\[ACPI\] Requesting S5 through uACPI namespace/FADT path" "${log}"; then
            echo "uACPI namespace/S5 path failed" >&2
            tail -n 400 "${log}" >&2
            return 1
        fi
    else
        if ! grep -q "\[uACPI\] Disabled by boot option; AML fallback only" "${log}" ||
           ! grep -q "\[ACPI\] AML fallback found _S5_" "${log}" ||
           ! grep -q "\[ACPI\] Fallback S5:" "${log}"; then
            echo "AML fallback or FADT GAS shutdown path failed" >&2
            tail -n 400 "${log}" >&2
            return 1
        fi
    fi

    rm -f "${iso}"
    grep -E "\[uACPI\]|\[ACPI\]|\[ACPI TEST\]|\[POWER\]" "${log}" || true
    echo "ACPI S5 ${kind} PASS"
}

run_s5_test acpi-uacpi
run_s5_test acpi-fallback
