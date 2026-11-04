#include "cpu.h"
#include "heap.h"
#include "limine.h"
#include "log.h"
#include "mm.h"
#include "pci.h"
#include "port.h"
#include "string.h"
#include "timer.h"
#include "vmm.h"

#include <uacpi/acpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/status.h>

extern volatile struct limine_rsdp_request rsdp_request;

typedef struct {
    uacpi_io_addr base;
    uacpi_size length;
} uacpi_io_mapping_t;

typedef struct {
    uacpi_pci_address address;
} uacpi_pci_handle_t;

typedef struct {
    volatile uacpi_u32 units;
} uacpi_semaphore_t;

static inline uacpi_u64 uacpi_read_tsc(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uacpi_u64)high << 32) | low;
}

static inline uacpi_interrupt_state uacpi_save_and_disable_interrupts(void) {
    uacpi_interrupt_state flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void uacpi_restore_interrupt_state(uacpi_interrupt_state flags) {
    if (flags & (1ull << 9)) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address) {
    if (!out_rsdp_address || !rsdp_request.response ||
        !rsdp_request.response->address) {
        return UACPI_STATUS_NOT_FOUND;
    }
    *out_rsdp_address = (uacpi_phys_addr)rsdp_request.response->address;
    return UACPI_STATUS_OK;
}

void* uacpi_kernel_map(uacpi_phys_addr address, uacpi_size length) {
    if (!length) return UACPI_MAP_FAILED;
    uint64_t aligned = address & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t offset = address - aligned;
    if (length > UINT64_MAX - offset) return UACPI_MAP_FAILED;
    uint64_t end = aligned + ((offset + length + PAGE_SIZE - 1) &
                              ~(uint64_t)(PAGE_SIZE - 1));
    if (end < aligned) return UACPI_MAP_FAILED;

    pagetable_t pml4 = vmm_get_kernel_pml4();
    if (!pml4) return UACPI_MAP_FAILED;
    for (uint64_t page = aligned; page < end; page += PAGE_SIZE) {
        uint64_t virt = phys_to_virt(page);
        if (!vmm_map_page(pml4, virt, page,
                          PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_NX)) {
            return UACPI_MAP_FAILED;
        }
    }
    return (void*)(uintptr_t)(phys_to_virt(aligned) + offset);
}

void uacpi_kernel_unmap(void* address, uacpi_size length) {
    (void)address;
    (void)length;
    /* Physical memory remains in the permanent kernel HHDM. */
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* message) {
    const char* prefix = "INFO";
    if (level == UACPI_LOG_ERROR) prefix = "ERROR";
    else if (level == UACPI_LOG_WARN) prefix = "WARN";
    else if (level == UACPI_LOG_TRACE) prefix = "TRACE";
    else if (level == UACPI_LOG_DEBUG) prefix = "DEBUG";
    log_printf("[uACPI %s] %s", prefix, message ? message : "(null)");
}

uacpi_status uacpi_kernel_pci_device_open(
    uacpi_pci_address address, uacpi_handle* out_handle) {
    if (!out_handle || address.segment != 0) {
        return UACPI_STATUS_NOT_FOUND;
    }
    uacpi_pci_handle_t* handle =
        (uacpi_pci_handle_t*)kmalloc(sizeof(*handle));
    if (!handle) return UACPI_STATUS_OUT_OF_MEMORY;
    handle->address = address;
    *out_handle = handle;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) {
    kfree(handle);
}

static bool uacpi_pci_offset_valid(uacpi_size offset) {
    return offset < 4096;
}

uacpi_status uacpi_kernel_pci_read8(
    uacpi_handle device, uacpi_size offset, uacpi_u8* value) {
    if (!device || !value || !uacpi_pci_offset_valid(offset)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uacpi_pci_handle_t* handle = (uacpi_pci_handle_t*)device;
    *value = pci_read8(handle->address.bus, handle->address.device,
                       handle->address.function, (uint16_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(
    uacpi_handle device, uacpi_size offset, uacpi_u16* value) {
    if (!device || !value || !uacpi_pci_offset_valid(offset) ||
        (offset & 1u)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uacpi_pci_handle_t* handle = (uacpi_pci_handle_t*)device;
    *value = pci_read16(handle->address.bus, handle->address.device,
                        handle->address.function, (uint16_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(
    uacpi_handle device, uacpi_size offset, uacpi_u32* value) {
    if (!device || !value || !uacpi_pci_offset_valid(offset) ||
        (offset & 3u)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uacpi_pci_handle_t* handle = (uacpi_pci_handle_t*)device;
    *value = pci_read32(handle->address.bus, handle->address.device,
                        handle->address.function, (uint16_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(
    uacpi_handle device, uacpi_size offset, uacpi_u8 value) {
    if (!device || !uacpi_pci_offset_valid(offset)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uacpi_pci_handle_t* handle = (uacpi_pci_handle_t*)device;
    pci_write8(handle->address.bus, handle->address.device,
               handle->address.function, (uint16_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(
    uacpi_handle device, uacpi_size offset, uacpi_u16 value) {
    if (!device || !uacpi_pci_offset_valid(offset) || (offset & 1u)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uacpi_pci_handle_t* handle = (uacpi_pci_handle_t*)device;
    pci_write16(handle->address.bus, handle->address.device,
                handle->address.function, (uint16_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(
    uacpi_handle device, uacpi_size offset, uacpi_u32 value) {
    if (!device || !uacpi_pci_offset_valid(offset) || (offset & 3u)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uacpi_pci_handle_t* handle = (uacpi_pci_handle_t*)device;
    pci_write32(handle->address.bus, handle->address.device,
                handle->address.function, (uint16_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(
    uacpi_io_addr base, uacpi_size length, uacpi_handle* out_handle) {
    if (!out_handle || !length || base > 0xFFFF ||
        length > 0x10000ull - base) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uacpi_io_mapping_t* mapping =
        (uacpi_io_mapping_t*)kmalloc(sizeof(*mapping));
    if (!mapping) return UACPI_STATUS_OUT_OF_MEMORY;
    mapping->base = base;
    mapping->length = length;
    *out_handle = mapping;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) {
    kfree(handle);
}

static bool uacpi_io_address(
    uacpi_handle handle, uacpi_size offset, uacpi_size width,
    uint16_t* out_port) {
    if (!handle || !out_port) return false;
    uacpi_io_mapping_t* mapping = (uacpi_io_mapping_t*)handle;
    if (offset > mapping->length || width > mapping->length - offset) {
        return false;
    }
    uacpi_io_addr address = mapping->base + offset;
    if (address > 0xFFFF || width > 0x10000ull - address) return false;
    *out_port = (uint16_t)address;
    return true;
}

uacpi_status uacpi_kernel_io_read8(
    uacpi_handle handle, uacpi_size offset, uacpi_u8* out_value) {
    uint16_t port;
    if (!out_value || !uacpi_io_address(handle, offset, 1, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inb(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(
    uacpi_handle handle, uacpi_size offset, uacpi_u16* out_value) {
    uint16_t port;
    if (!out_value || !uacpi_io_address(handle, offset, 2, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inw(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(
    uacpi_handle handle, uacpi_size offset, uacpi_u32* out_value) {
    uint16_t port;
    if (!out_value || !uacpi_io_address(handle, offset, 4, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    *out_value = inl(port);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(
    uacpi_handle handle, uacpi_size offset, uacpi_u8 value) {
    uint16_t port;
    if (!uacpi_io_address(handle, offset, 1, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    outb(port, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(
    uacpi_handle handle, uacpi_size offset, uacpi_u16 value) {
    uint16_t port;
    if (!uacpi_io_address(handle, offset, 2, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    outw(port, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(
    uacpi_handle handle, uacpi_size offset, uacpi_u32 value) {
    uint16_t port;
    if (!uacpi_io_address(handle, offset, 4, &port)) {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    outl(port, value);
    return UACPI_STATUS_OK;
}

void* uacpi_kernel_alloc(uacpi_size size) {
    if (!size || size > SIZE_MAX) return NULL;
    return kmalloc((size_t)size);
}

void* uacpi_kernel_alloc_zeroed(uacpi_size size) {
    if (!size || size > SIZE_MAX) return NULL;
    return kzalloc((size_t)size);
}

void uacpi_kernel_free(void* memory) {
    kfree(memory);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    if (!g_cpu_info.mhz) return (uacpi_u64)(global_tick) * 10000000ull;
    return (uacpi_read_tsc() * 1000ull) / g_cpu_info.mhz;
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    if (!usec) return;
    uacpi_u64 cycles = (uacpi_u64)g_cpu_info.mhz * usec;
    uacpi_u64 start = uacpi_read_tsc();
    while (uacpi_read_tsc() - start < cycles) {
        __asm__ volatile ("pause");
    }
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    if (!msec) return;
    uacpi_u64 cycles = (uacpi_u64)g_cpu_info.mhz * msec * 1000ull;
    uacpi_u64 start = uacpi_read_tsc();
    while (uacpi_read_tsc() - start < cycles) {
        __asm__ volatile ("pause");
    }
}

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial_units) {
    uacpi_semaphore_t* semaphore =
        (uacpi_semaphore_t*)kzalloc(sizeof(*semaphore));
    if (semaphore) semaphore->units = initial_units;
    return semaphore;
}

void uacpi_kernel_free_semaphore(uacpi_handle handle) {
    kfree(handle);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    return (uacpi_thread_id)(uintptr_t)1;
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    return uacpi_save_and_disable_interrupts();
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    uacpi_restore_interrupt_state(state);
}

uacpi_status uacpi_kernel_wait_for_semaphore(
    uacpi_handle handle, uacpi_u16 timeout) {
    if (!handle) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_semaphore_t* semaphore = (uacpi_semaphore_t*)handle;
    uacpi_u64 waited_us = 0;
    while (true) {
        uacpi_u32 units = __atomic_load_n(&semaphore->units, __ATOMIC_ACQUIRE);
        if (units && __atomic_compare_exchange_n(
                         &semaphore->units, &units, units - 1, false,
                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return UACPI_STATUS_OK;
        }
        if (timeout == 0 ||
            (timeout != 0xFFFF &&
             waited_us >= (uacpi_u64)timeout * 1000ull)) {
            return UACPI_STATUS_TIMEOUT;
        }
        uacpi_kernel_stall(100);
        waited_us += 100;
    }
}

void uacpi_kernel_signal_semaphore(uacpi_handle handle) {
    if (!handle) return;
    uacpi_semaphore_t* semaphore = (uacpi_semaphore_t*)handle;
    __atomic_fetch_add(&semaphore->units, 1, __ATOMIC_RELEASE);
}

uacpi_status uacpi_kernel_handle_firmware_request(
    uacpi_firmware_request* request) {
    if (!request) return UACPI_STATUS_INVALID_ARGUMENT;
    log_printf("[uACPI] Firmware request type=%u\n", request->type);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle context,
    uacpi_handle* out_irq_handle) {
    (void)irq;
    (void)handler;
    (void)context;
    if (!out_irq_handle) return UACPI_STATUS_INVALID_ARGUMENT;
    *out_irq_handle = (uacpi_handle)(uintptr_t)1;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle) {
    (void)handler;
    (void)irq_handle;
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    return (uacpi_handle)(uintptr_t)1;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) {
    (void)handle;
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) {
    (void)handle;
    return uacpi_save_and_disable_interrupts();
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    (void)handle;
    uacpi_restore_interrupt_state(flags);
}

uacpi_handle uacpi_kernel_create_work_item(void) {
    return (uacpi_handle)(uintptr_t)1;
}

void uacpi_kernel_free_work_item(uacpi_handle handle) {
    (void)handle;
}

void uacpi_kernel_schedule_work(
    uacpi_work_type type, uacpi_handle work_item,
    uacpi_work_handler handler, uacpi_handle context) {
    (void)type;
    (void)work_item;
    if (handler) handler(context);
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    return UACPI_STATUS_OK;
}
