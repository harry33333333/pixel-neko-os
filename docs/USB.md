# Pixel Neko USB Stack

## Controller Matrix

| Controller | Transport structures | Input path | Teardown |
| --- | --- | --- | --- |
| UHCI 1.1 | 4 KiB frame list, QH/TD rings | Interrupt TD polling | Stop schedule, clear slots |
| OHCI 1.1 | 256-byte-aligned HCCA, ED/TD chains | Interrupt TD polling | Remove ED references |
| EHCI 2.0 | Async QH and periodic frame list | High-speed interrupt QH | Remove periodic QH, IAAD doorbell |
| xHCI 3.0 | DCBAA, command/event/transfer TRBs, Slot/Endpoint Contexts | Interrupt Transfer Ring | Stop Endpoint, Disable Slot |

All controller descriptors contain physical DMA addresses. Kernel virtual
addresses are never written into host-controller memory.

## Device And Input Layers

The common USB core owns address allocation, descriptor parsing,
configuration selection, hub enumeration and detach propagation. USB devices
are also inserted into the P4a Bus/Device/Driver tree.

USB HID keyboard and mouse reports are translated into the same
`input_event_t` queue used by PS/2. Boot keyboard reports use the fixed
8-byte format. HID mouse interfaces probe their report descriptor for a wheel
usage and remain in report protocol when one is present.

Hub state follows:

```text
connected -> debounce (100 ms) -> reset -> recovery -> enumerate
```

Hub status-change endpoints are polled when available, with a bounded
GET_STATUS fallback. Root and hub-port removal recursively releases child
devices and their controller transfer resources.

## Validation

`scripts/qemu_smoke_test.sh` runs:

- UHCI with USB keyboard and mouse
- OHCI with USB keyboard and mouse
- EHCI with USB keyboard and mouse
- xHCI with USB keyboard and mouse
- xHCI plus external hub, downstream HID, QMP hot-add and hot-remove

The job fails unless both the controller-specific enumeration logs and USBHID
input reports are present, in addition to the P3/P4a regression checks.
