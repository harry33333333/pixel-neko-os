#ifndef _PXE_MEM_H
#define _PXE_MEM_H

#include <stddef.h>
#include <stdint.h>

void* pxe_sbrk(intptr_t delta);
void* malloc(size_t size);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);
void  free(void* ptr);

#endif /* _PXE_MEM_H */

