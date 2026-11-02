#ifndef _HEAP_H
#define _HEAP_H

#include <stddef.h>
#include <stdint.h>

void kheap_init(void);
void* kmalloc(size_t size);
void* kzalloc(size_t size);
void kfree(void* ptr);

#endif /* _HEAP_H */
