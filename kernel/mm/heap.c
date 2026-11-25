#include "heap.h"
#include "pmm.h"
#include "spinlock.h"
#include "log.h"
#include "string.h"

#define HEAP_MAGIC 0xDEADBEEF
#define HEAP_ALIGNMENT 16
#define ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))

typedef struct heap_block {
    size_t size;              // Usable payload size
    bool is_free;             // Free flag
    struct heap_block* next;  // Next block in chain
    struct heap_block* prev;  // Previous block in chain
    uint32_t magic;           // Magic sanity check
} __attribute__((aligned(HEAP_ALIGNMENT))) heap_block_t;

static spinlock_t g_heap_lock = SPINLOCK_INIT;
static heap_block_t* g_heap_start = NULL;

void kheap_init(void) {
    spin_lock_init(&g_heap_lock);
    size_t initial_pages = 256;
    phys_addr_t initial_phys = pmm_alloc_pages(initial_pages);
    if (!initial_phys) {
        log_printf("[HEAP FATAL] Failed to allocate initial pages for kernel heap!\n");
        return;
    }

    g_heap_start = (heap_block_t*)phys_to_virt(initial_phys);
    g_heap_start->size = (initial_pages * PAGE_SIZE) - sizeof(heap_block_t);
    g_heap_start->is_free = true;
    g_heap_start->next = NULL;
    g_heap_start->prev = NULL;
    g_heap_start->magic = HEAP_MAGIC;

    log_printf("[HEAP] Kernel Heap Initialized: Base %p, Initial size: %llu KB\n",
               g_heap_start, (uint64_t)(g_heap_start->size / 1024));
}

static void* kmalloc_from_block(heap_block_t* curr, size_t size) {
    if (curr->size >= size + sizeof(heap_block_t) + HEAP_ALIGNMENT) {
        heap_block_t* new_block = (heap_block_t*)((uint8_t*)(curr + 1) + size);
        new_block->size = curr->size - size - sizeof(heap_block_t);
        new_block->is_free = true;
        new_block->magic = HEAP_MAGIC;
        new_block->next = curr->next;
        new_block->prev = curr;

        if (curr->next) {
            curr->next->prev = new_block;
        }
        curr->next = new_block;
        curr->size = size;
    }
    curr->is_free = false;
    return (void*)(curr + 1);
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN_UP(size, HEAP_ALIGNMENT);

    uint64_t flags = spin_lock_irqsave(&g_heap_lock);
    heap_block_t* curr = g_heap_start;
    while (curr) {
        if (curr->magic != HEAP_MAGIC) {
            spin_unlock_irqrestore(&g_heap_lock, flags);
            log_printf("[HEAP ERROR] Heap corruption detected at %p (magic=0x%08x)!\n", curr, curr->magic);
            return NULL;
        }

        if (curr->is_free && curr->size >= size) {
            void* ptr = kmalloc_from_block(curr, size);
            spin_unlock_irqrestore(&g_heap_lock, flags);
            return ptr;
        }

        if (!curr->next) break;
        curr = curr->next;
    }

    size_t needed = size + sizeof(heap_block_t);
    size_t pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 64) pages = 64;

    phys_addr_t new_phys = pmm_alloc_pages(pages);
    if (!new_phys) {
        spin_unlock_irqrestore(&g_heap_lock, flags);
        log_printf("[HEAP ERROR] Out of physical memory while expanding heap!\n");
        return NULL;
    }

    heap_block_t* expanded = (heap_block_t*)phys_to_virt(new_phys);
    expanded->size = (pages * PAGE_SIZE) - sizeof(heap_block_t);
    expanded->is_free = true;
    expanded->magic = HEAP_MAGIC;
    expanded->next = NULL;
    expanded->prev = curr;

    if (curr) {
        curr->next = expanded;
    } else {
        g_heap_start = expanded;
    }

    void* ptr = kmalloc_from_block(expanded, size);
    spin_unlock_irqrestore(&g_heap_lock, flags);
    return ptr;
}

void* kzalloc(size_t size) {
    void* ptr = kmalloc(size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void* ptr) {
    if (!ptr) return;

    uint64_t flags = spin_lock_irqsave(&g_heap_lock);
    heap_block_t* block = (heap_block_t*)ptr - 1;
    if (block->magic != HEAP_MAGIC) {
        spin_unlock_irqrestore(&g_heap_lock, flags);
        log_printf("[HEAP ERROR] kfree(): Invalid pointer or heap corruption at %p!\n", ptr);
        return;
    }

    block->is_free = true;

    if (block->next && block->next->is_free &&
        ((uint8_t*)(block + 1) + block->size == (uint8_t*)block->next)) {
        block->size += sizeof(heap_block_t) + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }

    if (block->prev && block->prev->is_free &&
        ((uint8_t*)(block->prev + 1) + block->prev->size == (uint8_t*)block)) {
        block->prev->size += sizeof(heap_block_t) + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
    }
    spin_unlock_irqrestore(&g_heap_lock, flags);
}

void kheap_get_stats(size_t* total_bytes, size_t* used_bytes, size_t* free_bytes) {
    size_t tot = 0, used = 0, fre = 0;
    uint64_t flags = spin_lock_irqsave(&g_heap_lock);
    heap_block_t* curr = g_heap_start;
    while (curr && curr->magic == HEAP_MAGIC) {
        tot += sizeof(heap_block_t) + curr->size;
        if (curr->is_free) fre += curr->size;
        else used += curr->size;
        curr = curr->next;
    }
    spin_unlock_irqrestore(&g_heap_lock, flags);
    if (total_bytes) *total_bytes = tot;
    if (used_bytes) *used_bytes = used;
    if (free_bytes) *free_bytes = fre;
}
