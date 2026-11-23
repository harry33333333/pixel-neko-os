#ifndef _HEAP_H
#define _HEAP_H

#include <stddef.h>
#include <stdint.h>

void kheap_init(void);
void* kmalloc(size_t size);
void* kzalloc(size_t size);
void kfree(void* ptr);
void kheap_get_stats(size_t* total_bytes, size_t* used_bytes, size_t* free_bytes);

#endif /* _HEAP_H */
