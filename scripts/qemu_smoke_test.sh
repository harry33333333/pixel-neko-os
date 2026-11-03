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
        p5)
            if [ ! -f ".p5-images/p5-data.img" ] ||
               [ ! -f ".p5-images/p5-format16.img" ] ||
               [ ! -f ".p5-images/p5-format32.img" ] ||
               [ ! -f ".p5-images/p5-exfat.img" ]; then
                echo "P5 storage fixtures are missing" >&2
                return 2
            fi
            boot_args=(
                -drive "if=none,id=bootcd,format=raw,readonly=on,file=${ISO}"
                -device ide-cd,drive=bootcd,bus=ide.0
                -drive "if=none,id=p5data,format=raw,file=.p5-images/p5-data.img"
                -device ide-hd,drive=p5data,bus=ide.1
                -drive "if=none,id=p5fmt16,format=raw,file=.p5-images/p5-format16.img"
                -device ide-hd,drive=p5fmt16,bus=ide.2
                -drive "if=none,id=p5fmt32,format=raw,file=.p5-images/p5-format32.img"
                -device ide-hd,drive=p5fmt32,bus=ide.3
                -boot order=d
            )
            ;;
        p6)
            if [ ! -f ".p6-images/p6-target.img" ]; then
                echo "P6 installation target fixture is missing" >&2
                return 2
            fi
            boot_args=(
                -drive "if=none,id=p6bootcd,format=raw,readonly=on,file=${ISO}"
                -device ide-cd,drive=p6bootcd,bus=ide.0
                -drive "if=none,id=p6target,format=raw,file=.p6-images/p6-target.img"
                -device ide-hd,drive=p6target,bus=ide.1
                -boot order=d
            )
            ;;
        p6-mbr)
            if [ ! -f ".p6-images/p6-mbr.img" ]; then
                echo "P6 MBR installation fixture is missing" >&2
                return 2
            fi
            boot_args=(
                -drive "if=none,id=p6bootcd,format=raw,readonly=on,file=${ISO}"
                -device ide-cd,drive=p6bootcd,bus=ide.0
                -drive "if=none,id=p6target,format=raw,file=.p6-images/p6-mbr.img"
                -device ide-hd,drive=p6target,bus=ide.1
                -boot order=d
            )
            ;;
        p6-fat16)
            if [ ! -f ".p6-images/p6-fat16.img" ]; then
                echo "P6 FAT16 installation fixture is missing" >&2
                return 2
            fi
            boot_args=(
                -drive "if=none,id=p6bootcd,format=raw,readonly=on,file=${ISO}"
                -device ide-cd,drive=p6bootcd,bus=ide.0
                -drive "if=none,id=p6target,format=raw,file=.p6-images/p6-fat16.img"
                -device ide-hd,drive=p6target,bus=ide.1
                -boot order=d
            )
            ;;
        p6-disk)
            if [ ! -f ".p6-images/p6-target.img" ]; then
                echo "P6 installed disk fixture is missing" >&2
                return 2
            fi
            boot_args=(
                -drive "if=none,id=p6target,format=raw,file=.p6-images/p6-target.img"
                -device ide-hd,drive=p6target,bus=ide.1
                -boot order=c
            )
            ;;
        p6-disk-mbr)
            if [ ! -f ".p6-images/p6-mbr.img" ]; then
                echo "P6 installed MBR disk is missing" >&2
                return 2
            fi
            boot_args=(
                -drive "if=none,id=p6target,format=raw,file=.p6-images/p6-mbr.img"
                -device ide-hd,drive=p6target,bus=ide.1
                -boot order=c
            )
            ;;
        p6-disk-fat16)
            if [ ! -f ".p6-images/p6-fat16.img" ]; then
                echo "P6 installed FAT16 disk is missing" >&2
                return 2
            fi
            boot_args=(
                -drive "if=none,id=p6target,format=raw,file=.p6-images/p6-fat16.img"
                -device ide-hd,drive=p6target,bus=ide.1
                -boot order=c
            )
            ;;
        *)
            echo "Unknown boot source: ${boot_source}" >&2
            return 2
            ;;
    esac

    rm -f "${log}" "${console}" "${qmp}"
    local timeout_seconds=28
    if [ "${mode}" = "p6-install" ] ||
       [ "${mode}" = "p6-install-mbr" ] ||
       [ "${mode}" = "p6-install-fat16" ]; then
        timeout_seconds=65
    fi
    set +e
    timeout --foreground "${timeout_seconds}s" qemu-system-x86_64 \
        -machine "${machine},accel=tcg" \
        -cpu max \
        -smp 1 \
        -m 512M \
        -display none \
        -monitor none \
        -qmp "unix:${qmp},server=on,wait=off" \
        -no-reboot \
        -serial "file:${log}" \
        "$@" \
        "${boot_args[@]}" >"${console}" 2>&1 &
    local qemu_pid=$!
    set -e

    if [ "${mode}" = "p6-install" ] ||
       [ "${mode}" = "p6-install-mbr" ] ||
       [ "${mode}" = "p6-install-fat16" ]; then
        if ! python3 scripts/qmp_boot_menu.py "${qmp}"; then
            echo "Could not select the Limine graphical installer entry" >&2
            kill "${qemu_pid}" 2>/dev/null || true
            wait "${qemu_pid}" 2>/dev/null || true
            return 1
        fi
        if [ "${mode}" = "p6-install-fat16" ] &&
           ! grep -q "Format target .* as FAT16" "${log}"; then
            echo "P6 installer did not use the selected FAT16 format" >&2
            return 1
        fi
    fi

    for _ in $(seq 1 300); do
        if [ "${mode}" = "p6-install" ] ||
           [ "${mode}" = "p6-install-mbr" ] ||
           [ "${mode}" = "p6-install-fat16" ]; then
            if grep -q "\[P6 \] Graphical installer mode active" "${log}" 2>/dev/null; then
                format_choice=fat32
                if [ "${mode}" = "p6-install-fat16" ]; then
                    format_choice=fat16
                fi
                if ! python3 scripts/qmp_p6_install.py "${qmp}" "${log}" \
                    "${format_choice}"; then
                    echo "P6 graphical installation automation failed" >&2
                    kill "${qemu_pid}" 2>/dev/null || true
                    wait "${qemu_pid}" 2>/dev/null || true
                    tail -n 300 "${log}" >&2 || true
                    return 1
                fi
                if [ "${mode}" != "p6-install" ]; then
                    cp qemu-p6-install-screen.ppm "qemu-${mode}-screen.ppm"
                fi
                break
            fi
        elif grep -q "P3 PASS: Ring 3, SYSCALL, FPU context, IPC queue and shared memory" "${log}" 2>/dev/null; then
            python3 scripts/qmp_input_test.py "${qmp}" "${mode}"
            break
        fi
        sleep 0.2
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
         [ "${mode}" = "p5-ehci" ] || [ "${mode}" = "p5-xhci-hs" ] ||
         [ "${mode}" = "p5" ] || [ "${mode}" = "p6-live" ] ||
         [ "${mode}" = "p6-install" ] ||
         [ "${mode}" = "p6-install-mbr" ] ||
         [ "${mode}" = "p6-install-fat16" ] ||
         [ "${mode}" = "p6-disk-uefi" ] ||
         [ "${mode}" = "usb-modern" ]; } &&
       ! grep -q "PCI backend=.*PCIe ECAM" "${log}"; then
        echo "UEFI Q35 run did not use PCIe ECAM" >&2
        tail -n 250 "${log}" >&2
        return 1
    fi
    if { [ "${mode}" = "bios" ] || [ "${mode}" = "ohci" ] ||
         [ "${mode}" = "usb-legacy" ] ||
         [ "${mode}" = "p6-disk-bios" ] ||
         [ "${mode}" = "p6-disk-mbr" ] ||
         [ "${mode}" = "p6-disk-fat16" ]; } &&
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
    if [ "${mode}" = "p5" ] &&
       { ! grep -q "\[P5 \] FAT read/write PASS" "${log}" ||
         ! grep -q "\[P5 \] exFAT read/write PASS" "${log}" ||
         ! grep -q "\[P5 \] FAT16 format PASS" "${log}" ||
         ! grep -q "\[P5 \] FAT32 format PASS" "${log}" ||
         ! grep -q "\[P5 \] Storage self-test PASS" "${log}" ||
         ! grep -q "\[AHCI\].*ATAPI" "${log}" ||
         ! grep -q "\[USBMS\] Attached BOT" "${log}"; }; then
        echo "P5 storage validation failed" >&2
        tail -n 350 "${log}" >&2
        return 1
    fi
    if [ "${mode}" = "p6-install" ] ||
       [ "${mode}" = "p6-install-mbr" ] ||
       [ "${mode}" = "p6-install-fat16" ]; then
        if ! grep -q "\[P6 \] Installer scan found [1-9]" "${log}" ||
           ! grep -q "\[P6 \] Installer target selected" "${log}" ||
           ! grep -q "\[P6 \] Limine BIOS deployed" "${log}" ||
           ! grep -q "\[P6 \] Installation complete on" "${log}"; then
            echo "P6 installer did not complete the UEFI/BIOS deployment" >&2
            tail -n 400 "${log}" >&2
            return 1
        fi
    elif [ "${mode}" = "p6-live" ] ||
         [ "${mode}" = "p6-disk-uefi" ] ||
         [ "${mode}" = "p6-disk-bios" ] ||
         [ "${mode}" = "p6-disk-mbr" ] ||
         [ "${mode}" = "p6-disk-fat16" ]; then
        if ! grep -q "\[P6 \] Full desktop, splash and nine applications ready" "${log}"; then
            echo "P6 full desktop was not reached" >&2
            tail -n 350 "${log}" >&2
            return 1
        fi
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

verify_p6_disk() {
    local image="$1"
    local label="$2"
    mcopy -o -i "${image}@@1048576" \
        "::/boot/limine/limine-bios.sys" p6-deployed-limine.sys
    if ! cmp -s boot/limine/limine-bios.sys p6-deployed-limine.sys; then
        echo "Installed limine-bios.sys does not match the source payload" >&2
        exit 1
    fi
    dd if="${image}" of="qemu-${label}-disk-head.bin" \
        bs=512 count=256 status=none
    mdir -i "${image}@@1048576" ::/boot/limine
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
if selected_mode p5-ehci; then
run_mode p5-ehci q35 cdrom -bios "${UEFI_FW}" \
    -device ich9-usb-ehci1,id=ehci \
    -drive "if=none,id=usbdata,format=raw,readonly=on,file=${ISO}" \
    -device usb-storage,drive=usbdata,bus=ehci.0,id=usb-storage \
    -device usb-kbd,bus=ehci.0,id=usb-kbd \
    -device usb-mouse,bus=ehci.0,id=usb-mouse
fi
if selected_mode p5-xhci-hs; then
run_mode p5-xhci-hs q35 cdrom -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-hub,bus=xhci.0,port=2,id=usbhub \
    -drive "if=none,id=usbdata,format=raw,readonly=on,file=${ISO}" \
    -device usb-storage,drive=usbdata,bus=xhci.0,port=2.1,id=usb-storage \
    -device usb-kbd,bus=xhci.0,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,id=usb-mouse
fi
if selected_mode p5; then
run_mode p5 q35 p5 -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -drive "if=none,id=p5exfat,format=raw,file=.p5-images/p5-exfat.img" \
    -device usb-storage,drive=p5exfat,bus=xhci.0,port=1,id=usb-storage \
    -device usb-kbd,bus=xhci.0,port=2,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,port=3,id=usb-mouse
fi
if selected_mode p6-live; then
run_mode p6-live q35 p5 -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0,port=2,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,port=3,id=usb-mouse
fi
if selected_mode p6-install; then
run_mode p6-install q35 p6 -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0,port=1,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,port=2,id=usb-mouse
verify_p6_disk .p6-images/p6-target.img p6
fi
if selected_mode p6-install-mbr; then
run_mode p6-install-mbr q35 p6-mbr -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0,port=1,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,port=2,id=usb-mouse
verify_p6_disk .p6-images/p6-mbr.img p6-mbr
fi
if selected_mode p6-install-fat16; then
run_mode p6-install-fat16 q35 p6-fat16 -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0,port=1,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,port=2,id=usb-mouse
verify_p6_disk .p6-images/p6-fat16.img p6-fat16
fi
if selected_mode p6-disk-uefi; then
run_mode p6-disk-uefi q35 p6-disk -bios "${UEFI_FW}" \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0,port=2,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,port=3,id=usb-mouse
fi
if selected_mode p6-disk-bios; then
run_mode p6-disk-bios pc p6-disk \
    -device piix3-usb-uhci,id=uhci \
    -device usb-kbd,bus=uhci.0,id=usb-kbd \
    -device usb-mouse,bus=uhci.0,id=usb-mouse
fi
if selected_mode p6-disk-mbr; then
run_mode p6-disk-mbr pc p6-disk-mbr \
    -device piix3-usb-uhci,id=uhci \
    -device usb-kbd,bus=uhci.0,id=usb-kbd \
    -device usb-mouse,bus=uhci.0,id=usb-mouse
fi
if selected_mode p6-disk-fat16; then
run_mode p6-disk-fat16 pc p6-disk-fat16 \
    -device piix3-usb-uhci,id=uhci \
    -device usb-kbd,bus=uhci.0,id=usb-kbd \
    -device usb-mouse,bus=uhci.0,id=usb-mouse
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
    -device usb-kbd,bus=xhci.0,port=2,id=usb-kbd \
    -device usb-mouse,bus=xhci.0,port=3,id=usb-mouse
fi
