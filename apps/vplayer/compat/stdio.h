#ifndef _VPLAYER_COMPAT_STDIO_H
#define _VPLAYER_COMPAT_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <pxe/str.h>

typedef struct __vplayer_file {
    int dummy;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE*  fopen(const char* path, const char* mode);
int    fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int    fseek(FILE* stream, long offset, int whence);
long   ftell(FILE* stream);
int    fflush(FILE* stream);
int    printf(const char* fmt, ...);
int    fprintf(FILE* stream, const char* fmt, ...);
int    sprintf(char* buf, const char* fmt, ...);
int    vsprintf(char* buf, const char* fmt, va_list ap);
int    sscanf(const char* str, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
