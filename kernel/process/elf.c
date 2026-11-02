#include "elf.h"
#include "gdt.h"
#include "pmm.h"
#include "string.h"
#include "log.h"
#include "neko_abi.h"

#define ELF_MAGIC0        0x7F
#define ELF_MAGIC1        'E'
#define ELF_MAGIC2        'L'
#define ELF_MAGIC3        'F'
#define ELFCLASS64        2
#define ELFDATA2LSB       1
#define EV_CURRENT        1
#define ET_EXEC           2
#define EM_X86_64         62
#define PT_LOAD           1
#define PF_X              (1U << 0)
#define PF_W              (1U << 1)
#define PF_R              (1U << 2)

#define USER_STACK_TOP    0x00007ffffff00000ULL
#define USER_STACK_PAGES  8

typedef struct {
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

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

static bool range_in_image(uint64_t offset, uint64_t length, size_t image_size) {
    return offset <= image_size && length <= image_size - offset;
}

bool elf_validate(const uint8_t* image, size_t size) {
    if (!image || size < sizeof(elf64_ehdr_t)) return false;
    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)image;

    if (ehdr->e_ident[0] != ELF_MAGIC0 ||
        ehdr->e_ident[1] != ELF_MAGIC1 ||
        ehdr->e_ident[2] != ELF_MAGIC2 ||
        ehdr->e_ident[3] != ELF_MAGIC3) {
        return false;
    }
    if (ehdr->e_ident[4] != ELFCLASS64 ||
        ehdr->e_ident[5] != ELFDATA2LSB ||
        ehdr->e_ident[6] != EV_CURRENT) {
        return false;
    }
    if (ehdr->e_type != ET_EXEC || ehdr->e_machine != EM_X86_64 ||
        ehdr->e_version != EV_CURRENT) {
        return false;
    }
    if (ehdr->e_phentsize < sizeof(elf64_phdr_t) || ehdr->e_phnum == 0 ||
        ehdr->e_phnum > 64) {
        return false;
    }
    if (!range_in_image(ehdr->e_phoff,
                        (uint64_t)ehdr->e_phentsize * ehdr->e_phnum, size)) {
        return false;
    }

    return ehdr->e_entry < NEKO_USER_LIMIT;
}

static bool ensure_user_page(process_t* process, uint64_t virt, uint64_t flags) {
    phys_addr_t existing = 0;
    if (vmm_virt_to_phys(process->pml4, virt, &existing)) {
        return true;
    }

    phys_addr_t page = pmm_alloc_page();
    if (!page) return false;
    memset(phys_to_virt(page), 0, PAGE_SIZE);

    uint64_t map_flags = PTE_PRESENT | PTE_USER;
    if (flags & PF_W) map_flags |= PTE_WRITABLE;
    if (!(flags & PF_X)) map_flags |= PTE_NX;

    return vmm_map_page(process->pml4, virt, page, map_flags);
}

static bool load_segment(process_t* process, const uint8_t* image, size_t image_size,
                         const elf64_phdr_t* phdr) {
    if (phdr->p_memsz == 0) return true;
    if (phdr->p_filesz > phdr->p_memsz) return false;
    if (phdr->p_vaddr >= NEKO_USER_LIMIT ||
        phdr->p_memsz > NEKO_USER_LIMIT - phdr->p_vaddr) {
        return false;
    }
    if (!range_in_image(phdr->p_offset, phdr->p_filesz, image_size)) {
        return false;
    }
    if (phdr->p_align > 1 && (phdr->p_align & (phdr->p_align - 1)) != 0) {
        return false;
    }

    uint64_t first = phdr->p_vaddr & PAGE_MASK;
    uint64_t last = (phdr->p_vaddr + phdr->p_memsz - 1) & PAGE_MASK;

    for (uint64_t page = first; ; page += PAGE_SIZE) {
        if (!ensure_user_page(process, page, phdr->p_flags)) {
            return false;
        }
        if (page == last) break;
    }

    size_t copied = 0;
    while (copied < phdr->p_filesz) {
        uint64_t virt = phdr->p_vaddr + copied;
        phys_addr_t phys = 0;
        if (!vmm_virt_to_phys(process->pml4, virt, &phys)) return false;

        size_t page_left = PAGE_SIZE - (virt & 0xFFFULL);
        size_t chunk = phdr->p_filesz - copied;
        if (chunk > page_left) chunk = page_left;
        memcpy(phys_to_virt(phys), image + phdr->p_offset + copied, chunk);
        copied += chunk;
    }

    log_printf("[ELF] LOAD vaddr=0x%016llx mem=%llu KB file=%llu KB flags=%c%c%c\n",
               phdr->p_vaddr, phdr->p_memsz / 1024, phdr->p_filesz / 1024,
               (phdr->p_flags & PF_R) ? 'R' : '-',
               (phdr->p_flags & PF_W) ? 'W' : '-',
               (phdr->p_flags & PF_X) ? 'X' : '-');
    return true;
}

static uint64_t build_initial_stack(process_t* process, const char* arg0,
                                    const char* arg1) {
    const uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
        phys_addr_t page = pmm_alloc_page();
        if (!page) return 0;
        memset(phys_to_virt(page), 0, PAGE_SIZE);
        if (!vmm_map_page(process->pml4, stack_bottom + i * PAGE_SIZE, page,
                          PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)) {
            return 0;
        }
    }

    uint64_t sp = USER_STACK_TOP;
    sp -= 256;
    size_t len0 = strlen(arg0 ? arg0 : "init") + 1;
    if (len0 > 256 || !process_copy_to_user(process, sp, arg0 ? arg0 : "init", len0)) {
        return 0;
    }
    uint64_t argv0 = sp;

    uint64_t argv1 = 0;
    uint64_t argc = 1;
    if (arg1 && arg1[0]) {
        sp -= 256;
        size_t len1 = strlen(arg1) + 1;
        if (len1 > 256 || !process_copy_to_user(process, sp, arg1, len1)) {
            return 0;
        }
        argv1 = sp;
        argc = 2;
    }

    sp = (sp - 32) & ~0xFULL;
    sp -= 8;
    uint64_t zero = 0;
    if (!process_copy_to_user(process, sp, &zero, sizeof(zero))) return 0;
    if (argc == 2) {
        sp -= 8;
        if (!process_copy_to_user(process, sp, &argv1, sizeof(argv1))) return 0;
    }
    sp -= 8;
    if (!process_copy_to_user(process, sp, &argv0, sizeof(argv0))) return 0;
    sp -= 8;
    if (!process_copy_to_user(process, sp, &argc, sizeof(argc))) return 0;

    return sp;
}

process_t* elf_load_process(const uint8_t* image, size_t size,
                            const char* name, const char* arg0,
                            const char* arg1, process_t* parent) {
    if (!elf_validate(image, size)) {
        log_printf("[ELF] Rejected invalid ELF image for %s.\n", name ? name : "user");
        return NULL;
    }

    const elf64_ehdr_t* ehdr = (const elf64_ehdr_t*)image;
    process_t* process = process_create_empty(name, parent);
    if (!process) return NULL;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t* phdr =
            (const elf64_phdr_t*)(image + ehdr->e_phoff +
                                  (uint64_t)i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;
        if (!load_segment(process, image, size, phdr)) {
            log_printf("[ELF] Failed to load program header %u for %s.\n",
                       i, name ? name : "user");
            return NULL;
        }
    }

    uint64_t user_rsp = build_initial_stack(process, arg0, arg1);
    if (!user_rsp) {
        log_printf("[ELF] Failed to construct user stack for %s.\n",
                   name ? name : "user");
        return NULL;
    }

    process_register(process);
    if (!process_create_user_thread(process, ehdr->e_entry, user_rsp)) {
        return NULL;
    }

    log_printf("[ELF] PID %u (%s) ready: entry=0x%016llx rsp=0x%016llx\n",
               process->pid, process->name, ehdr->e_entry, user_rsp);
    return process;
}
