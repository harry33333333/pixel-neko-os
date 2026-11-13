#ifndef PDV_DDK_H
#define PDV_DDK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "format.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

#define PDV_PAGE_SIZE       4096ULL
#define PDV_DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1ULL))

/* --------------------------------------------------------------------------
 * Ring-0 Inline Port I/O & MMIO Helpers
 * -------------------------------------------------------------------------- */
static inline void pdv_mmio_barrier(void) {
    __asm__ __volatile__("" : : : "memory");
}

static inline uint8_t pdv_mmio_read8(volatile const void* address) {
    pdv_mmio_barrier();
    uint8_t value = *(volatile const uint8_t*)address;
    pdv_mmio_barrier();
    return value;
}

static inline uint16_t pdv_mmio_read16(volatile const void* address) {
    pdv_mmio_barrier();
    uint16_t value = *(volatile const uint16_t*)address;
    pdv_mmio_barrier();
    return value;
}

static inline uint32_t pdv_mmio_read32(volatile const void* address) {
    pdv_mmio_barrier();
    uint32_t value = *(volatile const uint32_t*)address;
    pdv_mmio_barrier();
    return value;
}

static inline void pdv_mmio_write8(volatile void* address, uint8_t value) {
    pdv_mmio_barrier();
    *(volatile uint8_t*)address = value;
    pdv_mmio_barrier();
}

static inline void pdv_mmio_write16(volatile void* address, uint16_t value) {
    pdv_mmio_barrier();
    *(volatile uint16_t*)address = value;
    pdv_mmio_barrier();
}

static inline void pdv_mmio_write32(volatile void* address, uint32_t value) {
    pdv_mmio_barrier();
    *(volatile uint32_t*)address = value;
    pdv_mmio_barrier();
}

static inline uint8_t pdv_mmio_read8_offset(volatile const void* base, uintptr_t offset) {
    return pdv_mmio_read8((volatile const uint8_t*)base + offset);
}

static inline uint16_t pdv_mmio_read16_offset(volatile const void* base, uintptr_t offset) {
    return pdv_mmio_read16((volatile const uint8_t*)base + offset);
}

static inline uint32_t pdv_mmio_read32_offset(volatile const void* base, uintptr_t offset) {
    return pdv_mmio_read32((volatile const uint8_t*)base + offset);
}

static inline void pdv_mmio_write8_offset(volatile void* base, uintptr_t offset, uint8_t value) {
    pdv_mmio_write8((volatile uint8_t*)base + offset, value);
}

static inline void pdv_mmio_write16_offset(volatile void* base, uintptr_t offset, uint16_t value) {
    pdv_mmio_write16((volatile uint8_t*)base + offset, value);
}

static inline void pdv_mmio_write32_offset(volatile void* base, uintptr_t offset, uint32_t value) {
    pdv_mmio_write32((volatile uint8_t*)base + offset, value);
}

static inline void pdv_outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t pdv_inb(uint16_t port) {
    uint8_t value;
    __asm__ __volatile__("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline uint16_t pdv_inw(uint16_t port) {
    uint16_t value;
    __asm__ __volatile__("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void pdv_outw(uint16_t port, uint16_t value) {
    __asm__ __volatile__("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t pdv_inl(uint16_t port) {
    uint32_t value;
    __asm__ __volatile__("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void pdv_outl(uint16_t port, uint32_t value) {
    __asm__ __volatile__("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* --------------------------------------------------------------------------
 * PCI, DMA, Framebuffer & Block Structures (ABI-identical to kernel structs)
 * -------------------------------------------------------------------------- */
#define PDV_PCI_BAR_COUNT 6

typedef struct {
    uint64_t address;
    uint64_t size;
    uint8_t  index;
    bool     is_io;
    bool     is_64bit;
    bool     prefetchable;
    bool     valid;
    bool     resource_conflict;
} pdv_pci_bar_t;

typedef struct {
    uint16_t      segment;
    uint8_t       bus;
    uint8_t       slot;
    uint8_t       function;
    uint16_t      vendor;
    uint16_t      device;
    uint16_t      subsystem_vendor;
    uint16_t      subsystem_device;
    uint8_t       revision;
    uint8_t       prog_if;
    uint8_t       subclass;
    uint8_t       class_code;
    uint8_t       header_type;
    uint8_t       irq_line;
    uint8_t       irq_pin;
    uint16_t      command;
    pdv_pci_bar_t bar[PDV_PCI_BAR_COUNT];
} pdv_pci_device_t;

typedef struct {
    union {
        uint64_t virt_addr;
        void*    virt;
    };
    union {
        uint64_t  phys_addr;
        uintptr_t phys;
    };
    uint64_t dma_addr;
    size_t   size;
    uint64_t dma_mask;
    size_t   alignment;
    int      is_coherent;
} pdv_dma_buffer_t;

typedef enum {
    PDV_PIXEL_FORMAT_RGB888 = 0,
    PDV_PIXEL_FORMAT_BGR888,
    PDV_PIXEL_FORMAT_RGBA8888,
    PDV_PIXEL_FORMAT_BGRA8888,
    PDV_PIXEL_FORMAT_RGB565,
    PDV_PIXEL_FORMAT_UNKNOWN
} pdv_pixel_format_t;

typedef struct {
    uint64_t           framebuffer_phys;
    void*              framebuffer_virt;
    uint32_t           width;
    uint32_t           height;
    uint32_t           pitch;
    uint32_t           bpp;
    pdv_pixel_format_t format;
    uint8_t            red_mask_size;
    uint8_t            red_mask_shift;
    uint8_t            green_mask_size;
    uint8_t            green_mask_shift;
    uint8_t            blue_mask_size;
    uint8_t            blue_mask_shift;
} pdv_fb_surface_t;

typedef enum {
    PDV_BLK_TYPE_NONE = 0,
    PDV_BLK_TYPE_FLOPPY,
    PDV_BLK_TYPE_ATA,
    PDV_BLK_TYPE_ATAPI,
    PDV_BLK_TYPE_AHCI,
    PDV_BLK_TYPE_USB,
    PDV_BLK_TYPE_MEMORY
} pdv_blk_type_t;

typedef enum {
    PDV_MEDIA_UNKNOWN = 0,
    PDV_MEDIA_PRESENT,
    PDV_MEDIA_ABSENT,
    PDV_MEDIA_ERROR
} pdv_media_state_t;

typedef struct pdv_blkdev {
    int               id;
    pdv_blk_type_t    type;
    int               unit;
    bool              is_removable;
    bool              is_readonly;
    uint32_t          sector_size;
    uint64_t          total_sectors;
    pdv_media_state_t media_state;
    uint32_t          media_generation;
    char              name[32];
    char              model[48];
    void*             priv;

    int (*read_sectors)(struct pdv_blkdev* dev, uint64_t lba, uint32_t count, void* buffer);
    int (*write_sectors)(struct pdv_blkdev* dev, uint64_t lba, uint32_t count, const void* buffer);
    pdv_media_state_t (*check_media)(struct pdv_blkdev* dev);
    int (*eject)(struct pdv_blkdev* dev);
} pdv_blkdev_t;

/* --------------------------------------------------------------------------
 * Subsystem Driver Registration Descriptors
 * -------------------------------------------------------------------------- */
typedef struct {
    const char* name;
    int         category;
    int         priority;
    int         (*probe)(void);
    int         (*start)(void);
    void        (*stop)(void);
} pdv_driver_descriptor_t;

#define PDV_DISPLAY_MIN_WIDTH   640
#define PDV_DISPLAY_MIN_HEIGHT  480
#define PDV_FALLBACK_MAX_WIDTH  1024
#define PDV_FALLBACK_MAX_HEIGHT 768
#define PDV_FALLBACK_MAX_BPP    16

#define PDV_BPP_MASK_8       (1U << 0)
#define PDV_BPP_MASK_16      (1U << 1)
#define PDV_BPP_MASK_24      (1U << 2)
#define PDV_BPP_MASK_32      (1U << 3)
#define PDV_BPP_MASK_ALL_STD (PDV_BPP_MASK_8 | PDV_BPP_MASK_16 | PDV_BPP_MASK_24 | PDV_BPP_MASK_32)

static inline uint8_t pdv_bpp_to_mask(int bpp) {
    if (bpp == 8)  return (uint8_t)PDV_BPP_MASK_8;
    if (bpp == 16) return (uint8_t)PDV_BPP_MASK_16;
    if (bpp == 24) return (uint8_t)PDV_BPP_MASK_24;
    if (bpp == 32) return (uint8_t)PDV_BPP_MASK_32;
    return 0U;
}

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bpp_mask;
    uint8_t  preferred;
    uint16_t reserved;
} pdv_display_mode_t;

typedef struct {
    const char*  name;
    int          priority;
    int          requires_hw_mode_set;
    int          (*probe)(void);
    int          (*start)(void);
    void         (*stop)(void);
    const char*  (*get_name)(void);
    const char*  (*get_status)(void);
    int          (*is_active)(void);
    int          (*is_2d_active)(void);
    unsigned int (*get_vram_size)(void);
    int          (*can_set_mode)(int w, int h, int bpp);
    int          (*supports_bpp)(int w, int h, int bpp);
    int          (*set_mode)(int w, int h, int bpp);
    void         (*fill_rect)(int x, int y, int w, int h, unsigned int color);
    void         (*screen_blt)(int sx, int sy, int dx, int dy, int w, int h);
    void         (*present_rect)(int x, int y, int w, int h);
    void         (*sync)(void);
    int          (*has_hw_cursor)(void);
    int          (*set_cursor)(int x, int y, int visible);
    int          (*get_mode_count)(void);
    int          (*get_mode)(int index, pdv_display_mode_t* out_mode); /* 1=ok, 0=err */
    int          (*get_max_width)(void);
    int          (*get_max_height)(void);
} pdv_display_driver_t;

typedef struct {
    const char*  name;
    int          priority;
    int          (*probe)(void);
    const char*  (*get_name)(void);
    int          (*is_present)(void);
    void         (*set_format)(int rate, int bits, int stereo);
    void         (*start_dma)(void);
    void         (*stop_dma)(void);
    void         (*pause_dma)(int pause);
    void         (*set_volume)(int master, int pcm);
    int          (*poll_half_needed)(void);
    void         (*write_half)(int half, const void* data, unsigned int bytes);
    void         (*silence_half)(int half);
    unsigned int (*get_dma_frame_pos)(void);
    const void*  (*get_dma_buffer)(void);
} pdv_audio_driver_t;

/* --------------------------------------------------------------------------
 * Kernel Driver Device Interface (DDI) Table v1.0
 * -------------------------------------------------------------------------- */
typedef struct pdv_kernel_api {
    uint32_t ddk_version;

    /* Logging */
    void (*log_printf)(const char* format, ...);

    /* Heap & DMA */
    void* (*kzalloc)(size_t size);
    void  (*kfree)(void* ptr);
    int   (*dma_alloc)(size_t size, uint64_t dma_mask, size_t alignment, pdv_dma_buffer_t* out_buf);
    void  (*dma_free)(pdv_dma_buffer_t* buf);
    void  (*dma_sync_for_cpu)(pdv_dma_buffer_t* buf);
    void  (*dma_sync_for_device)(pdv_dma_buffer_t* buf);

    /* ISA DMA Channel 2 */
    bool (*isadma_setup_read)(void* buffer, unsigned int length);
    bool (*isadma_setup_write)(const void* buffer, unsigned int length);
    void (*isadma_disable)(void);

    /* MMIO & Port I/O */
    volatile void* (*mmio_map_uc)(uint64_t phys, size_t size);
    volatile void* (*mmio_map_wc)(uint64_t phys, size_t size);
    uint8_t  (*inb)(uint16_t port);
    void     (*outb)(uint16_t port, uint8_t val);
    uint16_t (*inw)(uint16_t port);
    void     (*outw)(uint16_t port, uint16_t val);
    uint32_t (*inl)(uint16_t port);
    void     (*outl)(uint16_t port, uint32_t val);

    /* PCI Configuration & Enumeration */
    int                     (*pci_get_device_count)(void);
    const pdv_pci_device_t* (*pci_get_device)(int index);
    const pdv_pci_device_t* (*pci_find_by_id)(uint16_t vendor, uint16_t device);
    const pdv_pci_device_t* (*pci_find_class)(uint8_t class_code, uint8_t subclass);
    uint32_t (*pci_config_read)(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset);
    void     (*pci_config_write)(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset, uint32_t value);
    uint16_t (*pci_enable_io)(const pdv_pci_device_t* dev);
    uint16_t (*pci_enable_memory)(const pdv_pci_device_t* dev);
    uint16_t (*pci_enable_bus_master)(const pdv_pci_device_t* dev);
    uint64_t (*pci_get_bar_size)(const pdv_pci_device_t* dev, int bar);
    uint64_t (*pci_get_bar_address)(const pdv_pci_device_t* dev, int bar);
    uint8_t  (*pci_find_capability)(const pdv_pci_device_t* dev, uint8_t capability);

    /* Timing & Boot Splash */
    void (*sleep_ms)(int ms);
    void (*delay_ms)(uint32_t ms);
    int  (*timer_get_ticks)(void);
    void (*boot_screen_tick)(void);

    /* Framebuffer Surface & Display Subsystem */
    void           (*fb_get_surface)(pdv_fb_surface_t* out_surface);
    void           (*fb_set_surface)(const pdv_fb_surface_t* surface, unsigned char* new_lfb);
    void           (*fb_get_desktop_mode)(int* out_w, int* out_h, int* out_bpp);
    unsigned char* (*fb_get_lfb)(void);
    void           (*fb_flush)(void);
    void           (*fb_apply_ati_cap)(void);
    int            (*register_display_driver)(const pdv_display_driver_t* drv);

    /* Audio Subsystem */
    int  (*register_audio_driver)(const pdv_audio_driver_t* drv);
    void (*audio_set_probe_status)(const char* status);

    /* Block Storage Subsystem */
    pdv_blkdev_t* (*block_register)(const char* name, pdv_blk_type_t type, int unit,
                                    uint32_t sector_size, uint64_t total_sectors,
                                    bool is_readonly, bool is_removable,
                                    int (*read_blocks)(pdv_blkdev_t*, uint64_t, uint32_t, void*),
                                    int (*write_blocks)(pdv_blkdev_t*, uint64_t, uint32_t, const void*),
                                    pdv_media_state_t (*check_media)(pdv_blkdev_t*),
                                    void* priv);
    void (*block_unregister)(pdv_blkdev_t* dev);
    void (*block_set_capacity)(pdv_blkdev_t* dev, uint64_t sectors, uint32_t sector_size);

    /* Generic Driver Status Table */
    int (*register_driver_descriptor)(const pdv_driver_descriptor_t* descriptor);
} pdv_kernel_api_t;

/* Convenience PCI byte/word helpers via kapi */
static inline uint8_t pdv_pci_read8(const pdv_kernel_api_t* kapi, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset) {
    uint32_t val = kapi->pci_config_read(bus, slot, function, (uint16_t)(offset & ~3u));
    return (uint8_t)((val >> ((offset & 3u) * 8u)) & 0xFFu);
}

static inline void pdv_pci_write8(const pdv_kernel_api_t* kapi, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset, uint8_t value) {
    uint32_t val = kapi->pci_config_read(bus, slot, function, (uint16_t)(offset & ~3u));
    uint32_t shift = (offset & 3u) * 8u;
    val = (val & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    kapi->pci_config_write(bus, slot, function, (uint16_t)(offset & ~3u), val);
}

static inline uint16_t pdv_pci_read16(const pdv_kernel_api_t* kapi, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset) {
    uint32_t val = kapi->pci_config_read(bus, slot, function, (uint16_t)(offset & ~3u));
    return (uint16_t)((val >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

static inline void pdv_pci_write16(const pdv_kernel_api_t* kapi, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset, uint16_t value) {
    uint32_t val = kapi->pci_config_read(bus, slot, function, (uint16_t)(offset & ~3u));
    uint32_t shift = (offset & 2u) * 8u;
    val = (val & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    kapi->pci_config_write(bus, slot, function, (uint16_t)(offset & ~3u), val);
}

static inline uint32_t pdv_pci_read32(const pdv_kernel_api_t* kapi, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset) {
    return kapi->pci_config_read(bus, slot, function, offset);
}

static inline void pdv_pci_write32(const pdv_kernel_api_t* kapi, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset, uint32_t value) {
    kapi->pci_config_write(bus, slot, function, offset, value);
}

static inline pdv_dma_buffer_t pdv_dma_alloc_aligned(const pdv_kernel_api_t* kapi, size_t size, size_t alignment) {
    pdv_dma_buffer_t buf;
    size_t align = alignment < 4096 ? 4096 : alignment;
    if (kapi->dma_alloc(size, PDV_DMA_BIT_MASK(32), align, &buf) != 0) {
        buf.virt_addr = 0;
        buf.phys_addr = 0;
        buf.dma_addr = 0;
        buf.size = 0;
        buf.dma_mask = 0;
        buf.alignment = 0;
        buf.is_coherent = 0;
    }
    return buf;
}

static inline int pdv_dma_is_valid(const pdv_dma_buffer_t* buf) {
    return buf && buf->virt_addr != 0 && buf->phys_addr != 0 && buf->size != 0;
}

static inline void pdv_dma_flush(const pdv_kernel_api_t* kapi, const pdv_dma_buffer_t* buf) {
    if (buf) kapi->dma_sync_for_device((pdv_dma_buffer_t*)buf);
}

/* Mandatory PDV entry point exported by every *.pdv binary */
int pdv_driver_entry(const pdv_kernel_api_t* kapi);

#endif /* PDV_DDK_H */
