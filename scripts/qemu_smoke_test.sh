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
    local machine="$2"
    local boot_source="$3"
    shift
    shift
    shift
    local log="qemu-${mode}-serial.log"
    local console="qemu-${mode}-console.log"
    local qmp="qemu-${mode}.qmp"
    local status=0
    local boot_args=()

    case "${boot_source}" in
        cdrom)
            boot_args=(-cdrom "${ISO}")
            ;;
        usb-uhci)
            boot_args=(
                -drive "if=none,id=bootusb,format=raw,readonly=on,file=${ISO}"
                -device usb-storage,drive=bootusb,bus=uhci.0,port=1
                -boot order=c
            )
            ;;
        usb-xhci)
            boot_args=(
                -drive "if=none,id=bootusb,format=raw,readonly=on,file=${ISO}"
                -device usb-storage,drive=bootusb,bus=xhci.0,port=1
                -boot order=c
            )
            ;;
        *)
            echo "Unknown boot source: ${boot_source}" >&2
            return 2
            ;;
    esac

    rm -f "${log}" "${console}" "${qmp}"
    set +e
    timeout --foreground 28s qemu-system-x86_64 \
        -machine "${machine},accel=tcg" \
        -cpu max \
        -smp 1 \
        -m 512M \
        -display none \
        -monitor none \
        -qmp "unix:${qmp},server=on,wait=off" \
        -no-reboot \
        -serial "file:${log}" \
        "${boot_args[@]}" \
        "$@" >"${console}" 2>&1 &
    local qemu_pid=$!
    set -e

    for _ in $(seq 1 180); do
        if grep -q "P3 PASS: Ring 3, SYSCALL, FPU context, IPC queue and shared memory" "${log}" 2>/dev/null; then
            python3 scripts/qmp_input_test.py "${qmp}" "${mode}"
            break
        fi
        sleep 0.1
    done

    set +e
    wait "${qemu_pid}"
    status=$?
    set -e

    if [ "${status}" -ne 0 ] && [ "${status}" -ne 124 ]; then
        echo "QEMU ${mode} mode exited with status ${status}" >&2
        cat "${console}" >&2 || true
        cat "${log}" >&2 || true
        return 1
    fi

    if ! grep -q "SYSTEM.*P4b milestone reached" "${log}" ||
       ! grep -q "Ring 3 processes" "${log}" ||
       ! grep -q "P3 NMI GS_BASE safety test PASS" "${log}" ||
       ! grep -q "P3 PASS: Ring 3, SYSCALL, FPU context, IPC queue and shared memory" "${log}" ||
       ! grep -q "Enumerated [1-9][0-9]* PCI/PCIe function" "${log}" ||
       ! grep -q "\[INPUT\] KEY_DOWN code=0x70 'p'" "${log}" ||
       ! grep -q "\[USBHID\] KEY_DOWN code=0x70 'p'" "${log}"; then
        echo "QEMU ${mode} mode did not reach the P4a/P4b bus/input self-test" >&2
        tail -n 250 "${log}" >&2 || true
        return 1
    fi
    if [ "${mode}" != "hub" ] && [ "${mode}" != "generic" ] &&
       { ! grep -q "\[INPUT\] MOUSE_MOVE" "${log}" ||
         ! grep -q "\[INPUT\] MOUSE_BUTTON" "${log}" ||
         ! grep -q "\[USBHID\] MOUSE" "${log}"; }; then
        echo "QEMU ${mode} mode did not receive USB mouse input" >&2
        tail -n 250 "${log}" >&2
        return 1
    fi

    if { [ "${mode}" = "uefi" ] || [ "${mode}" = "ehci" ] ||
         [ "${mode}" = "hub" ] || [ "${mode}" = "generic" ] ||
         [ "${mode}" = "usb-modern" ]; } &&
       ! grep -q "PCI backend=.*PCIe ECAM" "${log}"; then
        echo "UEFI Q35 run did not use PCIe ECAM" >&2
        tail -n 250 "${log}" >&2
        return 1
    fi
    if { [ "${mode}" = "bios" ] || [ "${mode}" = "ohci" ] ||
         [ "${mode}" = "usb-legacy" ]; } &&
       ! grep -q "PCI backend=.*CF8/CFC" "${log}"; then
        echo "Legacy run did not use CF8/CFC configuration space" >&2
        tail -n 250 "${log}" >&2
        return 1
    fi
    if grep -q "Resource conflict:" "${log}"; then
        echo "P4a detected an unexpected BAR resource conflict" >&2
        grep "Resource conflict:" "${log}" >&2
        return 1
    fi
    if [ "${mode}" = "hub" ]; then
        attached_keyboards=$(
            grep -c "\[USBHID\] Attached keyboard" "${log}" || true
        )
        if [ "${attached_keyboards}" -lt 2 ] ||
           ! grep -q "\[HUB \] Attached" "${log}" ||
           ! grep -q "\[HUB \] Device connected" "${log}" ||
           ! grep -q "\[HUB \] Port enabled" "${log}" ||
           ! grep -q "\[USBHID\] Attached mouse" "${log}" ||
           ! grep -q "\[INPUT\] MOUSE_MOVE" "${log}" ||
           ! grep -q "\[INPUT\] MOUSE_BUTTON" "${log}" ||
           ! grep -q "\[USBHID\] KEY_DOWN code=0x61 'a'" "${log}" ||
           ! grep -q "\[USBHOT\] Root port .* disconnected address" "${log}"; then
            echo "Hub or USB hotplug validation failed" >&2
            cat "${console}" >&2 || true
            tail -n 300 "${log}" >&2
            return 1
        fi
    fi
    if [ "${mode}" = "generic" ] &&
       { ! grep -q "Descriptor parser self-test PASS" "${log}" ||
         ! grep -q "Attached mouse.*mode=report" "${log}" ||
         ! grep -q "\[USBHID\] BUTTON buttons=1" "${log}" ||
         ! grep -q "\[INPUT\] MOUSE_BUTTON" "${log}"; }; then
        echo "Generic non-Boot HID validation failed" >&2
        tail -n 300 "${log}" >&2
        return 1
    fi
    if [ ! -s "qemu-${mode}-screen.ppm" ]; then
        echo "QEMU ${mode} did not produce a framebuffer screenshot" >&2
        return 1
    fi

    echo "QEMU ${mode} boot and P4b USB input self-test passed."
}

selected_mode() {
    local wanted="$1"
    [ "${QEMU_MODES:-all}" = "all" ] && return 0
    case ",${QEMU_MODES}," in
        *",${wanted},"*) return 0 ;;
        *) return 1 ;;
    esac
}

if selected_mode uefi; then
run_mode uefi q35 cdrom -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,id=usb-mouse
fi
if selected_mode bios; then
run_mode bios pc cdrom -boot d \
    -device piix3-usb-uhci,id=uhci \
    -device usb-kbd,bus=uhci.0,id=usb-kbd \
    -device usb-mouse,bus=uhci.0,id=usb-mouse
fi
if selected_mode ohci; then
run_mode ohci pc cdrom -boot d \
    -device pci-ohci,id=ohci \
    -device usb-kbd,bus=ohci.0,id=usb-kbd \
    -device usb-mouse,bus=ohci.0,id=usb-mouse
fi
if selected_mode ehci; then
run_mode ehci q35 cdrom -bios "${UEFI_FW}" \
    -device ich9-usb-ehci1,id=ehci \
    -device usb-kbd,bus=ehci.0,id=usb-kbd \
    -device usb-mouse,bus=ehci.0,id=usb-mouse
fi
if selected_mode hub; then
run_mode hub q35 cdrom -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-hub,bus=xhci.0,port=3,id=usbhub \
    -device usb-kbd,bus=xhci.0,port=3.1,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,port=3.2,id=usb-mouse
fi
if selected_mode generic; then
run_mode generic q35 cdrom -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0,id=usb-kbd \
    -device usb-tablet,bus=xhci.0,id=usb-tablet
fi
if selected_mode usb-legacy; then
run_mode usb-legacy pc usb-uhci \
    -device piix3-usb-uhci,id=uhci \
    -device usb-hub,bus=uhci.0,port=2,id=usbhub \
    -device usb-kbd,bus=uhci.0,port=2.1,id=usb-kbd \
    -device usb-mouse,bus=uhci.0,port=2.2,id=usb-mouse
fi
if selected_mode usb-modern; then
run_mode usb-modern q35 usb-xhci -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,id=usb-mouse
fi
