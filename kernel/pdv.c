#include "pdv.h"
#include "blkdev.h"
#include "boot_framebuffer.h"
#include "dma.h"
#include "driver.h"
#include "heap.h"
#include "isadma.h"
#include "log.h"
#include "mm.h"
#include "mmio.h"
#include "pci.h"
#include "pmm.h"
#include "port.h"
#include "settings.h"
#include "string.h"
#include "timer.h"
#include "usb_core.h"
#include "vfs.h"
#include "vmm.h"

/* Ensure 1:1 binary layout compatibility between DDK structs and kernel structs */
_Static_assert(sizeof(pdv_pci_bar_t) == sizeof(pci_bar_t), "pdv_pci_bar_t ABI mismatch");
_Static_assert(sizeof(pdv_pci_device_t) == sizeof(pci_device_t), "pdv_pci_device_t ABI mismatch");
_Static_assert(sizeof(pdv_dma_buffer_t) == sizeof(dma_buffer_t), "pdv_dma_buffer_t ABI mismatch");
_Static_assert(sizeof(pdv_fb_surface_t) == sizeof(gfx_surface_t), "pdv_fb_surface_t ABI mismatch");
_Static_assert(sizeof(pdv_blkdev_t) == sizeof(blkdev_t), "pdv_blkdev_t ABI mismatch");
_Static_assert(sizeof(pdv_driver_descriptor_t) == sizeof(driver_descriptor_t), "pdv_driver_descriptor_t ABI mismatch");

extern unsigned char* lfb;
extern int screen_w;
extern int screen_h;
extern int screen_bpp;

/* High-half kernel module virtual arena (within -mcmodel=kernel [-2GB, -1] & pml4[511]) */
#define PDV_ARENA_BASE  0xFFFFFFFF90000000ULL
#define PDV_ARENA_END   0xFFFFFFFFC0000000ULL

/* ELF64 ET_REL definitions */
#define ELF_MAGIC_0     0x7FU
#define ELF_MAGIC_1     'E'
#define ELF_MAGIC_2     'L'
#define ELF_MAGIC_3     'F'
#define ELFCLASS64      2U
#define ELFDATA2LSB     1U
#define ET_REL          1U
#define EM_X86_64       62U

#define SHT_NULL        0U
#define SHT_PROGBITS    1U
#define SHT_SYMTAB      2U
#define SHT_STRTAB      3U
#define SHT_RELA        4U
#define SHT_NOBITS      8U

#define SHF_WRITE       0x1ULL
#define SHF_ALLOC       0x2ULL
#define SHF_EXECINSTR   0x4ULL

#define SHN_UNDEF       0U
#define SHN_ABS         0xFFF1U
#define SHN_COMMON      0xFFF2U

#define STB_GLOBAL      1U
#define STB_WEAK        2U

#define R_X86_64_NONE   0U
#define R_X86_64_64     1U
#define R_X86_64_PC32   2U
#define R_X86_64_PLT32  4U
#define R_X86_64_32     10U
#define R_X86_64_32S    11U
#define R_X86_64_PC64   24U

#define PDV_MAX_SECTIONS 128
#define PDV_MAX_COMMON   64
#define PDV_DIR_MAX_ENTRIES 64

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct __attribute__((packed)) {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_shdr_t;

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} elf64_sym_t;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} elf64_rela_t;

typedef int (*pdv_entry_fn_t)(const pdv_kernel_api_t* kapi);

static uint64_t g_pdv_next_virt = PDV_ARENA_BASE;
static pdv_module_info_t g_loaded_pdvs[PDV_MAX_LOADED];
static int g_loaded_pdv_count = 0;

/* --------------------------------------------------------------------------
 * Kernel DDI Wrapper Functions
 * -------------------------------------------------------------------------- */
static int ddi_dma_alloc(size_t size, uint64_t dma_mask, size_t alignment, pdv_dma_buffer_t* out_buf) {
    return dma_alloc(size, dma_mask, alignment, (dma_buffer_t*)out_buf);
}

static void ddi_dma_free(pdv_dma_buffer_t* buf) {
    dma_free((dma_buffer_t*)buf);
}

static void ddi_dma_sync_cpu(pdv_dma_buffer_t* buf) {
    dma_sync_for_cpu((dma_buffer_t*)buf);
}

static void ddi_dma_sync_dev(pdv_dma_buffer_t* buf) {
    dma_sync_for_device((dma_buffer_t*)buf);
}

static volatile void* ddi_mmio_map_uc(uint64_t phys, size_t size) {
    return mmio_map_phys(phys, size);
}

static volatile void* ddi_mmio_map_wc(uint64_t phys, size_t size) {
    return mmio_map_wc(phys, size);
}

static uint8_t  ddi_inb(uint16_t port) { return inb(port); }
static void     ddi_outb(uint16_t port, uint8_t val) { outb(port, val); }
static uint16_t ddi_inw(uint16_t port) { return inw(port); }
static void     ddi_outw(uint16_t port, uint16_t val) { outw(port, val); }
static uint32_t ddi_inl(uint16_t port) { return inl(port); }
static void     ddi_outl(uint16_t port, uint32_t val) { outl(port, val); }

static const pdv_pci_device_t* ddi_pci_get_device(int index) {
    return (const pdv_pci_device_t*)pci_get_device(index);
}

static const pdv_pci_device_t* ddi_pci_find_by_id(uint16_t vendor, uint16_t device) {
    return (const pdv_pci_device_t*)pci_find_by_id(vendor, device);
}

static const pdv_pci_device_t* ddi_pci_find_class(uint8_t class_code, uint8_t subclass) {
    return (const pdv_pci_device_t*)pci_find_class(class_code, subclass);
}

static uint16_t ddi_pci_enable_io(const pdv_pci_device_t* dev) {
    return pci_enable_io((const pci_device_t*)dev);
}

static uint16_t ddi_pci_enable_memory(const pdv_pci_device_t* dev) {
    return pci_enable_memory((const pci_device_t*)dev);
}

static uint16_t ddi_pci_enable_bus_master(const pdv_pci_device_t* dev) {
    return pci_enable_bus_master((const pci_device_t*)dev);
}

static uint64_t ddi_pci_get_bar_size(const pdv_pci_device_t* dev, int bar) {
    return pci_get_bar_size((const pci_device_t*)dev, bar);
}

static uint64_t ddi_pci_get_bar_address(const pdv_pci_device_t* dev, int bar) {
    return pci_get_bar_address((const pci_device_t*)dev, bar);
}

static uint8_t ddi_pci_find_capability(const pdv_pci_device_t* dev, uint8_t capability) {
    return pci_find_capability((const pci_device_t*)dev, capability);
}

static void ddi_fb_get_surface(pdv_fb_surface_t* out_surface) {
    if (out_surface) {
        memcpy(out_surface, &g_main_surface, sizeof(pdv_fb_surface_t));
    }
}

static void ddi_fb_set_surface(const pdv_fb_surface_t* surface, unsigned char* new_lfb) {
    if (surface) {
        memcpy(&g_main_surface, surface, sizeof(gfx_surface_t));
    }
    if (new_lfb) {
        lfb = new_lfb;
    }
}

static void ddi_fb_get_desktop_mode(int* out_w, int* out_h, int* out_bpp) {
    if (out_w) *out_w = screen_w;
    if (out_h) *out_h = screen_h;
    if (out_bpp) *out_bpp = screen_bpp;
}

static unsigned char* ddi_fb_get_lfb(void) {
    return lfb;
}

static pdv_blkdev_t* ddi_block_register(
    const char* name, pdv_blk_type_t type, int unit,
    uint32_t sector_size, uint64_t total_sectors,
    bool is_readonly, bool is_removable,
    int (*read_blocks)(pdv_blkdev_t*, uint64_t, uint32_t, void*),
    int (*write_blocks)(pdv_blkdev_t*, uint64_t, uint32_t, const void*),
    pdv_media_state_t (*check_media)(pdv_blkdev_t*),
    void* priv) {
    return (pdv_blkdev_t*)block_register(
        name, (blk_type_t)type, unit, sector_size, total_sectors,
        is_readonly, is_removable,
        (int (*)(blkdev_t*, uint64_t, uint32_t, void*))read_blocks,
        (int (*)(blkdev_t*, uint64_t, uint32_t, const void*))write_blocks,
        (media_state_t (*)(blkdev_t*))check_media,
        priv);
}

static void ddi_block_unregister(pdv_blkdev_t* dev) {
    block_unregister((blkdev_t*)dev);
}

static void ddi_block_set_capacity(pdv_blkdev_t* dev, uint64_t sectors, uint32_t sector_size) {
    block_set_capacity((blkdev_t*)dev, sectors, sector_size);
}

static int ddi_register_driver_descriptor(const pdv_driver_descriptor_t* desc) {
    return driver_register((const driver_descriptor_t*)desc);
}

static const pdv_kernel_api_t g_pdv_kernel_api = {
    .ddk_version              = PDV_DDK_VERSION_1_0,
    .log_printf               = log_printf,
    .kzalloc                  = kzalloc,
    .kfree                    = kfree,
    .dma_alloc                = ddi_dma_alloc,
    .dma_free                 = ddi_dma_free,
    .dma_sync_for_cpu         = ddi_dma_sync_cpu,
    .dma_sync_for_device      = ddi_dma_sync_dev,
    .isadma_setup_read        = isadma_setup_read,
    .isadma_setup_write       = isadma_setup_write,
    .isadma_disable           = isadma_disable,
    .mmio_map_uc              = ddi_mmio_map_uc,
    .mmio_map_wc              = ddi_mmio_map_wc,
    .inb                      = ddi_inb,
    .outb                     = ddi_outb,
    .inw                      = ddi_inw,
    .outw                     = ddi_outw,
    .inl                      = ddi_inl,
    .outl                     = ddi_outl,
    .pci_get_device_count     = pci_get_device_count,
    .pci_get_device           = ddi_pci_get_device,
    .pci_find_by_id           = ddi_pci_find_by_id,
    .pci_find_class           = ddi_pci_find_class,
    .pci_config_read          = pci_config_read,
    .pci_config_write         = pci_config_write,
    .pci_enable_io            = ddi_pci_enable_io,
    .pci_enable_memory        = ddi_pci_enable_memory,
    .pci_enable_bus_master    = ddi_pci_enable_bus_master,
    .pci_get_bar_size         = ddi_pci_get_bar_size,
    .pci_get_bar_address      = ddi_pci_get_bar_address,
    .pci_find_capability      = ddi_pci_find_capability,
    .sleep_ms                 = sleep_ms,
    .delay_ms                 = usb_delay_ms,
    .timer_get_ticks          = timer_get_ticks,
    .boot_screen_tick         = boot_screen_tick,
    .fb_get_surface           = ddi_fb_get_surface,
    .fb_set_surface           = ddi_fb_set_surface,
    .fb_get_desktop_mode      = ddi_fb_get_desktop_mode,
    .fb_get_lfb               = ddi_fb_get_lfb,
    .fb_flush                 = boot_framebuffer_flush,
    .fb_apply_ati_cap         = gfx_accel_notify_ati_cap,
    .register_display_driver  = gfx_accel_register_pdv_driver,
    .register_audio_driver    = audio_register_pdv_driver,
    .audio_set_probe_status   = audio_set_probe_status,
    .block_register           = ddi_block_register,
    .block_unregister         = ddi_block_unregister,
    .block_set_capacity       = ddi_block_set_capacity,
    .register_driver_descriptor = ddi_register_driver_descriptor,
};

/* --------------------------------------------------------------------------
 * CRC32, String & Hardware Matching Helpers
 * -------------------------------------------------------------------------- */
static uint32_t pdv_crc32_compute(const uint8_t* buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = (i >= 28 && i < 32) ? 0U : buf[i];
        crc ^= byte;
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static void pdv_safe_str(char* dst, size_t dst_cap, const char* src, size_t src_cap) {
    if (!dst || dst_cap == 0) return;
    size_t i = 0;
    while (i + 1 < dst_cap && i < src_cap && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static char pdv_tolower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool pdv_ends_with_pdv(const char* name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 5) return false;
    return name[len - 4] == '.' &&
           pdv_tolower(name[len - 3]) == 'p' &&
           pdv_tolower(name[len - 2]) == 'd' &&
           pdv_tolower(name[len - 1]) == 'v';
}

static bool pdv_rule_matches_pci(const pdv_match_rule_t* rule) {
    int count = pci_get_device_count();
    for (int i = 0; i < count; i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev) continue;
        if (rule->vendor_id != PDV_WILDCARD_U16 && dev->vendor != rule->vendor_id) continue;
        if (rule->device_id != PDV_WILDCARD_U16 && dev->device != rule->device_id) continue;
        if (rule->subsys_vendor != PDV_WILDCARD_U16 && dev->subsystem_vendor != rule->subsys_vendor) continue;
        if (rule->subsys_device != PDV_WILDCARD_U16 && dev->subsystem_device != rule->subsys_device) continue;
        if (rule->pci_class != PDV_WILDCARD_U8 && dev->class_code != rule->pci_class) continue;
        if (rule->pci_subclass != PDV_WILDCARD_U8 && dev->subclass != rule->pci_subclass) continue;
        if (rule->pci_progif != PDV_WILDCARD_U8 && dev->prog_if != rule->pci_progif) continue;
        return true;
    }
    return false;
}

static bool pdv_rule_matches_isa(const pdv_match_rule_t* rule) {
    if (rule->isa_base_port == PDV_WILDCARD_U16) return true;
    if (rule->isa_base_port == 0x03F0U) {
        /* Standard PC floppy controller: MSR at 0x3F4 reads != 0xFF when present */
        return inb(0x03F4U) != 0xFFU;
    }
    return inb(rule->isa_base_port) != 0xFFU;
}

static bool pdv_matches_hardware(const pdv_header_t* hdr, const pdv_match_rule_t* rules) {
    for (uint32_t i = 0; i < hdr->match_count; i++) {
        const pdv_match_rule_t* r = &rules[i];
        switch (r->match_type) {
            case PDV_MATCH_ANY:
                return true;
            case PDV_MATCH_PCI_ID:
            case PDV_MATCH_PCI_SUBSYS:
            case PDV_MATCH_PCI_CLASS:
            case PDV_MATCH_PCI_VEN_CLASS:
                if (pdv_rule_matches_pci(r)) return true;
                break;
            case PDV_MATCH_ISA_PNP:
                if (pdv_rule_matches_isa(r)) return true;
                break;
            default:
                break;
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * ELF64 ET_REL Loader with Strict W^X Segment Mapping
 * -------------------------------------------------------------------------- */
static uint64_t align_up_u64(uint64_t val, uint64_t align) {
    if (align <= 1) return val;
    if ((align & (align - 1)) != 0) {
        uint64_t p = 1;
        while (p < align) p <<= 1;
        align = p;
    }
    return (val + align - 1ULL) & ~(align - 1ULL);
}

static uint64_t pdv_resolve_builtin(const char* name) {
    if (!name) return 0;
    if (strcmp(name, "memcpy") == 0)  return (uint64_t)(uintptr_t)&memcpy;
    if (strcmp(name, "memset") == 0)  return (uint64_t)(uintptr_t)&memset;
    if (strcmp(name, "memmove") == 0) return (uint64_t)(uintptr_t)&memmove;
    if (strcmp(name, "memcmp") == 0)  return (uint64_t)(uintptr_t)&memcmp;
    if (strcmp(name, "strlen") == 0)  return (uint64_t)(uintptr_t)&strlen;
    if (strcmp(name, "strcmp") == 0)  return (uint64_t)(uintptr_t)&strcmp;
    if (strcmp(name, "strncmp") == 0) return (uint64_t)(uintptr_t)&strncmp;
    return 0;
}

typedef struct {
    uint32_t sym_idx;
    uint64_t vaddr;
} pdv_common_sym_t;

typedef struct {
    uint64_t    sec_vaddr[PDV_MAX_SECTIONS];
    uint8_t*    sec_hptr[PDV_MAX_SECTIONS];
    int         sec_seg[PDV_MAX_SECTIONS];
    uint64_t    sec_off[PDV_MAX_SECTIONS];
    pdv_common_sym_t commons[PDV_MAX_COMMON];
    int         common_count;
} pdv_elf_ctx_t;

static int pdv_load_and_start_elf(const uint8_t* elf_buf, size_t elf_size,
                                  const char* filename, const pdv_header_t* hdr) {
    if (elf_size < sizeof(elf64_ehdr_t)) return -1;
    const elf64_ehdr_t* eh = (const elf64_ehdr_t*)elf_buf;
    if (eh->e_ident[0] != ELF_MAGIC_0 || eh->e_ident[1] != ELF_MAGIC_1 ||
        eh->e_ident[2] != ELF_MAGIC_2 || eh->e_ident[3] != ELF_MAGIC_3 ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_type != ET_REL || eh->e_machine != EM_X86_64) {
        log_printf("[PDV ] %s: invalid ELF64 ET_REL header\n", filename);
        return -1;
    }
    if (eh->e_shnum == 0 || eh->e_shnum > PDV_MAX_SECTIONS ||
        eh->e_shentsize < sizeof(elf64_shdr_t) ||
        eh->e_shoff > elf_size ||
        (uint64_t)eh->e_shnum * eh->e_shentsize > elf_size - eh->e_shoff) {
        log_printf("[PDV ] %s: invalid ELF64 section table\n", filename);
        return -1;
    }

    pdv_elf_ctx_t* ctx = (pdv_elf_ctx_t*)kzalloc(sizeof(pdv_elf_ctx_t));
    if (!ctx) return -1;

    const elf64_shdr_t* shdrs = (const elf64_shdr_t*)(elf_buf + eh->e_shoff);
    const elf64_shdr_t* symtab_sh = NULL;
    const char* strtab = NULL;
    size_t strtab_size = 0;

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        ctx->sec_seg[i] = -1;
        const elf64_shdr_t* sh = &shdrs[i];
        if (sh->sh_type != SHT_NOBITS && sh->sh_size > 0) {
            if (sh->sh_offset > elf_size || sh->sh_size > elf_size - sh->sh_offset) {
                log_printf("[PDV ] %s: section %u out of bounds\n", filename, i);
                kfree(ctx);
                return -1;
            }
        }
        if ((sh->sh_flags & (SHF_WRITE | SHF_EXECINSTR)) == (SHF_WRITE | SHF_EXECINSTR)) {
            log_printf("[PDV ] %s: section %u violates W^X\n", filename, i);
            kfree(ctx);
            return -1;
        }
        if (sh->sh_type == SHT_SYMTAB) {
            symtab_sh = sh;
        }
    }

    if (!symtab_sh || symtab_sh->sh_link >= eh->e_shnum ||
        symtab_sh->sh_entsize < sizeof(elf64_sym_t)) {
        log_printf("[PDV ] %s: missing valid symbol table\n", filename);
        kfree(ctx);
        return -1;
    }
    const elf64_shdr_t* str_sh = &shdrs[symtab_sh->sh_link];
    if (str_sh->sh_type != SHT_STRTAB || str_sh->sh_offset > elf_size ||
        str_sh->sh_size > elf_size - str_sh->sh_offset) {
        kfree(ctx);
        return -1;
    }
    strtab = (const char*)(elf_buf + str_sh->sh_offset);
    strtab_size = (size_t)str_sh->sh_size;

    /* Group SHF_ALLOC sections into 0=RX, 1=RO, 2=RW */
    uint64_t seg_size[3] = { 0, 0, 0 };
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const elf64_shdr_t* sh = &shdrs[i];
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) continue;
        int seg = 1;
        if (sh->sh_flags & SHF_EXECINSTR) seg = 0;
        else if (sh->sh_flags & SHF_WRITE) seg = 2;

        uint64_t align = sh->sh_addralign ? sh->sh_addralign : 1ULL;
        seg_size[seg] = align_up_u64(seg_size[seg], align);
        ctx->sec_seg[i] = seg;
        ctx->sec_off[i] = seg_size[seg];
        seg_size[seg] += sh->sh_size;
    }

    /* Append SHN_COMMON symbols to RW segment */
    uint32_t sym_count = (uint32_t)(symtab_sh->sh_size / symtab_sh->sh_entsize);
    for (uint32_t s = 1; s < sym_count; s++) {
        const elf64_sym_t* sym =
            (const elf64_sym_t*)(elf_buf + symtab_sh->sh_offset + (uint64_t)s * symtab_sh->sh_entsize);
        if (sym->st_shndx == SHN_COMMON && sym->st_size > 0) {
            if (ctx->common_count >= PDV_MAX_COMMON) {
                kfree(ctx);
                return -1;
            }
            uint64_t align = sym->st_value ? sym->st_value : 8ULL;
            seg_size[2] = align_up_u64(seg_size[2], align);
            ctx->commons[ctx->common_count].sym_idx = s;
            ctx->commons[ctx->common_count].vaddr = seg_size[2]; /* relative offset in RW */
            ctx->common_count++;
            seg_size[2] += sym->st_size;
        }
    }

    size_t seg_pages[3] = {
        (size_t)((seg_size[0] + PAGE_SIZE - 1ULL) / PAGE_SIZE),
        (size_t)((seg_size[1] + PAGE_SIZE - 1ULL) / PAGE_SIZE),
        (size_t)((seg_size[2] + PAGE_SIZE - 1ULL) / PAGE_SIZE),
    };
    size_t total_pages = seg_pages[0] + seg_pages[1] + seg_pages[2];
    if (total_pages == 0 || seg_pages[0] == 0) {
        log_printf("[PDV ] %s: empty executable text segment\n", filename);
        kfree(ctx);
        return -1;
    }

    uint64_t needed_virt = ((uint64_t)total_pages + 1ULL) * PAGE_SIZE; /* +1 guard page */
    if (g_pdv_next_virt + needed_virt > PDV_ARENA_END) {
        log_printf("[PDV ] %s: module virtual arena exhausted\n", filename);
        kfree(ctx);
        return -1;
    }

    phys_addr_t seg_phys[3] = { 0, 0, 0 };
    for (int g = 0; g < 3; g++) {
        if (seg_pages[g] == 0) continue;
        seg_phys[g] = pmm_alloc_pages(seg_pages[g]);
        if (!seg_phys[g]) {
            for (int k = 0; k < g; k++) {
                if (seg_phys[k]) pmm_free_pages(seg_phys[k], seg_pages[k]);
            }
            kfree(ctx);
            return -1;
        }
        memset(phys_to_virt(seg_phys[g]), 0, seg_pages[g] * PAGE_SIZE);
    }

    uint64_t mod_base = g_pdv_next_virt;
    uint64_t cursor = mod_base;
    uint64_t seg_vbase[3] = { 0, 0, 0 };
    for (int g = 0; g < 3; g++) {
        seg_vbase[g] = cursor;
        cursor += (uint64_t)seg_pages[g] * PAGE_SIZE;
    }

    /* Assign section virtual addresses and copy SHT_PROGBITS via HHDM */
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const elf64_shdr_t* sh = &shdrs[i];
        if (!(sh->sh_flags & SHF_ALLOC)) continue;
        if (sh->sh_size == 0) {
            ctx->sec_vaddr[i] = mod_base;
            ctx->sec_hptr[i] = NULL;
            continue;
        }
        int g = ctx->sec_seg[i];
        ctx->sec_vaddr[i] = seg_vbase[g] + ctx->sec_off[i];
        ctx->sec_hptr[i] = (uint8_t*)phys_to_virt(seg_phys[g]) + ctx->sec_off[i];
        if (sh->sh_type != SHT_NOBITS) {
            memcpy(ctx->sec_hptr[i], elf_buf + sh->sh_offset, (size_t)sh->sh_size);
        }
    }

    for (int c = 0; c < ctx->common_count; c++) {
        ctx->commons[c].vaddr += seg_vbase[2];
    }

    /* Apply SHT_RELA relocations through HHDM before mapping read-only/exec PTEs */
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const elf64_shdr_t* rsh = &shdrs[i];
        if (rsh->sh_type != SHT_RELA || rsh->sh_size == 0) continue;
        if (rsh->sh_info >= eh->e_shnum || rsh->sh_entsize < sizeof(elf64_rela_t)) continue;

        uint32_t target_sec = rsh->sh_info;
        const elf64_shdr_t* tsh = &shdrs[target_sec];
        if (!(tsh->sh_flags & SHF_ALLOC) || tsh->sh_size == 0 || !ctx->sec_hptr[target_sec]) {
            continue;
        }

        uint32_t rela_count = (uint32_t)(rsh->sh_size / rsh->sh_entsize);
        for (uint32_t r = 0; r < rela_count; r++) {
            const elf64_rela_t* rela =
                (const elf64_rela_t*)(elf_buf + rsh->sh_offset + (uint64_t)r * rsh->sh_entsize);
            uint32_t sym_idx = (uint32_t)(rela->r_info >> 32);
            uint32_t r_type  = (uint32_t)(rela->r_info & 0xFFFFFFFFU);
            if (r_type == R_X86_64_NONE) continue;
            if (sym_idx >= sym_count || rela->r_offset >= tsh->sh_size) goto fail_free_phys;

            const elf64_sym_t* sym =
                (const elf64_sym_t*)(elf_buf + symtab_sh->sh_offset + (uint64_t)sym_idx * symtab_sh->sh_entsize);
            const char* sym_name = (sym->st_name < strtab_size) ? (strtab + sym->st_name) : "";

            uint64_t S = 0;
            if (sym->st_shndx == SHN_UNDEF) {
                S = pdv_resolve_builtin(sym_name);
                if (!S) {
                    log_printf("[PDV ] %s: unresolved symbol '%s'\n", filename, sym_name);
                    goto fail_free_phys;
                }
            } else if (sym->st_shndx == SHN_ABS) {
                S = sym->st_value;
            } else if (sym->st_shndx == SHN_COMMON) {
                for (int c = 0; c < ctx->common_count; c++) {
                    if (ctx->commons[c].sym_idx == sym_idx) {
                        S = ctx->commons[c].vaddr;
                        break;
                    }
                }
                if (!S) goto fail_free_phys;
            } else if (sym->st_shndx < eh->e_shnum) {
                S = ctx->sec_vaddr[sym->st_shndx] + sym->st_value;
            } else {
                goto fail_free_phys;
            }

            uint64_t P = ctx->sec_vaddr[target_sec] + rela->r_offset;
            uint8_t* patch = ctx->sec_hptr[target_sec] + rela->r_offset;
            int64_t A = rela->r_addend;

            switch (r_type) {
                case R_X86_64_64:
                    if (rela->r_offset + 8 > tsh->sh_size) goto fail_free_phys;
                    *(uint64_t*)patch = S + (uint64_t)A;
                    break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32: {
                    if (rela->r_offset + 4 > tsh->sh_size) goto fail_free_phys;
                    int64_t disp = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                    if (disp < -2147483648LL || disp > 2147483647LL) goto fail_free_phys;
                    *(int32_t*)patch = (int32_t)disp;
                    break;
                }
                case R_X86_64_32: {
                    if (rela->r_offset + 4 > tsh->sh_size) goto fail_free_phys;
                    uint64_t val = S + (uint64_t)A;
                    if (val > 0xFFFFFFFFULL) goto fail_free_phys;
                    *(uint32_t*)patch = (uint32_t)val;
                    break;
                }
                case R_X86_64_32S: {
                    if (rela->r_offset + 4 > tsh->sh_size) goto fail_free_phys;
                    int64_t val = (int64_t)(S + (uint64_t)A);
                    if (val < -2147483648LL || val > 2147483647LL) goto fail_free_phys;
                    *(int32_t*)patch = (int32_t)val;
                    break;
                }
                case R_X86_64_PC64:
                    if (rela->r_offset + 8 > tsh->sh_size) goto fail_free_phys;
                    *(int64_t*)patch = (int64_t)(S + (uint64_t)A) - (int64_t)P;
                    break;
                default:
                    log_printf("[PDV ] %s: unsupported reloc type %u\n", filename, r_type);
                    goto fail_free_phys;
            }
        }
    }

    /* Locate pdv_driver_entry */
    uint64_t entry_vaddr = 0;
    for (uint32_t s = 1; s < sym_count; s++) {
        const elf64_sym_t* sym =
            (const elf64_sym_t*)(elf_buf + symtab_sh->sh_offset + (uint64_t)s * symtab_sh->sh_entsize);
        if (sym->st_name >= strtab_size) continue;
        uint8_t bind = sym->st_info >> 4;
        if ((bind == STB_GLOBAL || bind == STB_WEAK) &&
            sym->st_shndx != SHN_UNDEF && sym->st_shndx < eh->e_shnum &&
            strcmp(strtab + sym->st_name, "pdv_driver_entry") == 0) {
            if (!(shdrs[sym->st_shndx].sh_flags & SHF_EXECINSTR)) break;
            entry_vaddr = ctx->sec_vaddr[sym->st_shndx] + sym->st_value;
            break;
        }
    }
    if (!entry_vaddr) {
        log_printf("[PDV ] %s: missing executable pdv_driver_entry\n", filename);
        goto fail_free_phys;
    }

    /* Map W^X virtual segments into kernel PML4 */
    pagetable_t kpml4 = vmm_get_kernel_pml4();
    const uint64_t seg_flags[3] = {
        PTE_PRESENT,                             /* Seg 0: RX */
        PTE_PRESENT | PTE_NX,                    /* Seg 1: RO */
        PTE_PRESENT | PTE_WRITABLE | PTE_NX      /* Seg 2: RW */
    };

    for (int g = 0; g < 3; g++) {
        for (size_t p = 0; p < seg_pages[g]; p++) {
            uint64_t va = seg_vbase[g] + (uint64_t)p * PAGE_SIZE;
            phys_addr_t pa = seg_phys[g] + (uint64_t)p * PAGE_SIZE;
            if (!vmm_map_page(kpml4, va, pa, seg_flags[g])) {
                for (int ug = 0; ug <= g; ug++) {
                    size_t limit = (ug == g) ? p : seg_pages[ug];
                    for (size_t up = 0; up < limit; up++) {
                        vmm_unmap_page(kpml4, seg_vbase[ug] + (uint64_t)up * PAGE_SIZE);
                    }
                }
                goto fail_free_phys;
            }
        }
    }

    g_pdv_next_virt = cursor + PAGE_SIZE; /* Reserve +1 guard page */
    kfree(ctx);
    ctx = NULL;

    __asm__ __volatile__("mfence" : : : "memory");

    pdv_entry_fn_t entry_fn = (pdv_entry_fn_t)(uintptr_t)entry_vaddr;
    int rc = entry_fn(&g_pdv_kernel_api);
    if (rc < 0) {
        log_printf("[PDV ] %s: pdv_driver_entry returned %d; unloading\n", filename, rc);
        for (int g = 0; g < 3; g++) {
            for (size_t p = 0; p < seg_pages[g]; p++) {
                vmm_unmap_page(kpml4, seg_vbase[g] + (uint64_t)p * PAGE_SIZE);
            }
            if (seg_phys[g]) pmm_free_pages(seg_phys[g], seg_pages[g]);
        }
        return -1;
    }

    if (g_loaded_pdv_count < PDV_MAX_LOADED) {
        pdv_module_info_t* info = &g_loaded_pdvs[g_loaded_pdv_count++];
        pdv_safe_str(info->filename, sizeof(info->filename), filename, 48);
        pdv_safe_str(info->driver_name, sizeof(info->driver_name), hdr->driver_name, sizeof(hdr->driver_name));
        pdv_safe_str(info->provider, sizeof(info->provider), hdr->provider, sizeof(hdr->provider));
        pdv_safe_str(info->version_str, sizeof(info->version_str), hdr->version_str, sizeof(hdr->version_str));
        info->driver_class = hdr->driver_class;
        info->base_virt = mod_base;
        info->rx_pages = seg_pages[0];
        info->ro_pages = seg_pages[1];
        info->rw_pages = seg_pages[2];
    }

    char dname[33];
    char dver[17];
    pdv_safe_str(dname, sizeof(dname), hdr->driver_name, sizeof(hdr->driver_name));
    pdv_safe_str(dver, sizeof(dver), hdr->version_str, sizeof(hdr->version_str));
    log_printf("[PDV ] Loaded %s (%s v%s) @ 0x%016llx [RX=%u RO=%u RW=%u pages]\n",
               filename, dname, dver, (unsigned long long)mod_base,
               (unsigned int)seg_pages[0], (unsigned int)seg_pages[1], (unsigned int)seg_pages[2]);
    return 0;

fail_free_phys:
    for (int g = 0; g < 3; g++) {
        if (seg_phys[g]) pmm_free_pages(seg_phys[g], seg_pages[g]);
    }
    if (ctx) kfree(ctx);
    return -1;
}

/* --------------------------------------------------------------------------
 * Public PDV Loader & PDEVICE\ Enumeration API
 * -------------------------------------------------------------------------- */
int pdv_load_file(const char* vfs_path, bool safe_mode, bool check_hw_match) {
    if (!vfs_path) return -1;

    vfs_dirent_t dirent;
    if (vfs_find_file(vfs_path, &dirent) != VFS_OK || dirent.is_dir ||
        dirent.size < sizeof(pdv_header_t) || dirent.size > 4U * 1024U * 1024U) {
        return -1;
    }

    size_t file_size = (size_t)dirent.size;
    size_t buf_pages = (file_size + PAGE_SIZE - 1ULL) / PAGE_SIZE;
    phys_addr_t buf_phys = pmm_alloc_pages(buf_pages);
    if (!buf_phys) return -1;

    uint8_t* buf = (uint8_t*)phys_to_virt(buf_phys);
    int bytes_read = vfs_read_file(vfs_path, buf, (unsigned int)file_size);
    if (bytes_read != (int)file_size) {
        pmm_free_pages(buf_phys, buf_pages);
        return -1;
    }

    const pdv_header_t* hdr = (const pdv_header_t*)buf;
    if (hdr->magic[0] != PDV_MAGIC_0 || hdr->magic[1] != PDV_MAGIC_1 ||
        hdr->magic[2] != PDV_MAGIC_2 || hdr->magic[3] != PDV_MAGIC_3 ||
        hdr->format_version != PDV_FORMAT_VERSION_1_0 ||
        hdr->min_ddk_version > PDV_DDK_VERSION_1_0 ||
        hdr->match_table_off != sizeof(pdv_header_t) ||
        hdr->match_count > 256U ||
        (hdr->elf_payload_off & (PAGE_SIZE - 1ULL)) != 0 ||
        hdr->elf_payload_size < sizeof(elf64_ehdr_t) ||
        hdr->elf_payload_off > file_size ||
        hdr->elf_payload_size > file_size - hdr->elf_payload_off ||
        hdr->match_table_off + hdr->match_count * sizeof(pdv_match_rule_t) > hdr->elf_payload_off) {
        log_printf("[PDV ] %s: header validation failed\n", vfs_path);
        pmm_free_pages(buf_phys, buf_pages);
        return -1;
    }

    uint32_t expected_crc = pdv_crc32_compute(buf, file_size);
    if (expected_crc != hdr->file_crc32) {
        log_printf("[PDV ] %s: CRC32 mismatch (expected 0x%08x, header 0x%08x)\n",
                   vfs_path, expected_crc, hdr->file_crc32);
        pmm_free_pages(buf_phys, buf_pages);
        return -1;
    }

    if (safe_mode && !(hdr->flags & PDV_FLAG_SAFE_MODE_OK)) {
        pmm_free_pages(buf_phys, buf_pages);
        return 1; /* Skipped by safe_mode policy */
    }

    const pdv_match_rule_t* rules = (const pdv_match_rule_t*)(buf + hdr->match_table_off);
    if (check_hw_match) {
        if (!(hdr->flags & PDV_FLAG_BOOT_START) || !pdv_matches_hardware(hdr, rules)) {
            pmm_free_pages(buf_phys, buf_pages);
            return 1; /* Hardware not present on this machine */
        }
    }

    /* Extract basename for logging */
    const char* base_name = vfs_path;
    for (const char* p = vfs_path; *p; p++) {
        if (*p == '\\' || *p == '/') base_name = p + 1;
    }

    int load_rc = pdv_load_and_start_elf(buf + hdr->elf_payload_off,
                                         hdr->elf_payload_size,
                                         base_name, hdr);
    pmm_free_pages(buf_phys, buf_pages);
    return load_rc;
}

typedef struct {
    char names[PDV_DIR_MAX_ENTRIES][64];
    int  count;
} pdv_dir_list_t;

static pdv_dir_list_t* g_pdv_dir_collector = NULL;

static void pdv_collect_dirent(const vfs_dirent_t* entry) {
    if (!g_pdv_dir_collector || !entry || entry->is_dir) return;
    if (!pdv_ends_with_pdv(entry->name)) return;
    if (g_pdv_dir_collector->count >= PDV_DIR_MAX_ENTRIES) return;
    pdv_safe_str(g_pdv_dir_collector->names[g_pdv_dir_collector->count],
                 sizeof(g_pdv_dir_collector->names[0]),
                 entry->name, sizeof(entry->name));
    g_pdv_dir_collector->count++;
}

static bool pdv_dir_exists(const char* dir_path) {
    vfs_dirent_t d;
    return vfs_find_file(dir_path, &d) == VFS_OK && d.is_dir;
}

static bool pdv_resolve_boot_pdevice_dir(char* out_dir, size_t cap) {
    if (!out_dir || cap < 16) return false;

    /* In local installed mode (mode=local), prioritize <SYS>:\PDEVICE even if
     * the Live CD ISO is still mounted in the optical drive. */
    char sys = settings_get_system_drive();
    if (sys != 0) {
        out_dir[0] = sys;
        out_dir[1] = ':';
        out_dir[2] = '\\';
        pdv_safe_str(out_dir + 3, cap - 3, "PDEVICE", 7);
        if (pdv_dir_exists(out_dir)) return true;
    }

    /* In Live / Installer mode (sys == 0), load from <CD>:\PDEVICE. */
    char cd = vfs_get_boot_cdrom_letter();
    if (cd != 0) {
        out_dir[0] = cd;
        out_dir[1] = ':';
        out_dir[2] = '\\';
        pdv_safe_str(out_dir + 3, cap - 3, "PDEVICE", 7);
        if (pdv_dir_exists(out_dir)) return true;
    }

    for (char letter = 'C'; letter <= 'Z'; letter++) {
        vfs_drive_t* drv = vfs_get_drive(letter);
        if (!drv || !drv->present || !drv->mounted) continue;
        if (sys != 0 && drv->drv_type == VFS_DRV_CDROM) continue;
        char kpath[32];
        kpath[0] = letter;
        kpath[1] = ':';
        pdv_safe_str(kpath + 2, sizeof(kpath) - 2, "\\boot\\kernel.elf", 16);
        vfs_dirent_t ke;
        if (vfs_find_file(kpath, &ke) != VFS_OK || ke.is_dir) continue;

        out_dir[0] = letter;
        out_dir[1] = ':';
        out_dir[2] = '\\';
        pdv_safe_str(out_dir + 3, cap - 3, "PDEVICE", 7);
        if (pdv_dir_exists(out_dir)) return true;
    }
    return false;
}

int pdv_autoload_boot_drivers(bool safe_mode, bool skip_all) {
    if (skip_all) {
        log_printf("[PDV ] Safe mode nopdv=1 active; skipping PDEVICE driver load.\n");
        return 0;
    }

    pci_scan_all();

    char pdevice_dir[32];
    if (!pdv_resolve_boot_pdevice_dir(pdevice_dir, sizeof(pdevice_dir))) {
        log_printf("[PDV ] No PDEVICE directory found on boot media.\n");
        return 0;
    }

    pdv_dir_list_t* list = (pdv_dir_list_t*)kzalloc(sizeof(pdv_dir_list_t));
    if (!list) return -1;

    g_pdv_dir_collector = list;
    int lrc = vfs_list_dir(pdevice_dir, pdv_collect_dirent);
    g_pdv_dir_collector = NULL;

    if (lrc != VFS_OK || list->count == 0) {
        log_printf("[PDV ] Directory %s contains 0 PDV packages.\n", pdevice_dir);
        kfree(list);
        return 0;
    }

    int loaded = 0;
    for (int i = 0; i < list->count; i++) {
        char full_path[128];
        size_t dlen = strlen(pdevice_dir);
        pdv_safe_str(full_path, sizeof(full_path), pdevice_dir, dlen);
        full_path[dlen] = '\\';
        pdv_safe_str(full_path + dlen + 1, sizeof(full_path) - dlen - 1,
                     list->names[i], strlen(list->names[i]));

        if (pdv_load_file(full_path, safe_mode, true) == 0) {
            loaded++;
        }
        boot_screen_tick();
    }

    log_printf("[PDV ] Scanned %s (%d package(s)), loaded %d matching driver(s).\n",
               pdevice_dir, list->count, loaded);
    kfree(list);
    return loaded;
}

int pdv_get_loaded_count(void) {
    return g_loaded_pdv_count;
}

const pdv_module_info_t* pdv_get_loaded_info(int index) {
    if (index < 0 || index >= g_loaded_pdv_count) return NULL;
    return &g_loaded_pdvs[index];
}

