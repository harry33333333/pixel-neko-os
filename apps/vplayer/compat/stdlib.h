#ifndef _VPLAYER_COMPAT_STDLIB_H
#define _VPLAYER_COMPAT_STDLIB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <pxe/mem.h>

static inline int abs(int x) { return x < 0 ? -x : x; }
static inline long labs(long x) { return x < 0 ? -x : x; }
static inline long long llabs(long long x) { return x < 0 ? -x : x; }

void  exit(int status);
void  abort(void);
void  qsort(void* base, size_t nmemb, size_t size,
            int (*compar)(const void*, const void*));
int   atoi(const char* s);
long  atol(const char* s);
long  strtol(const char* nptr, char** endptr, int base);

#ifdef __cplusplus
}
#endif

#endif
