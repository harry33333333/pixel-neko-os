#ifndef PIXEL_NEKO_LIBCPUID_STDIO_H
#define PIXEL_NEKO_LIBCPUID_STDIO_H

#include <stdarg.h>

typedef struct pixel_neko_libcpuid_file FILE;

#define stdin  ((FILE*)0)
#define stdout ((FILE*)0)
#define stderr ((FILE*)0)

int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);

#endif
