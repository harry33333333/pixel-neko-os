#ifndef _PXE_STR_H
#define _PXE_STR_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* dst, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);

size_t strlen(const char* s);
char*  strcpy(char* dst, const char* src);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strcat(char* dst, const char* src);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcasecmp(const char* a, const char* b);
char*  strchr(const char* s, int c);
char*  strrchr(const char* s, int c);

int    vsnprintf(char* buf, size_t cap, const char* fmt, va_list ap);
int    snprintf(char* buf, size_t cap, const char* fmt, ...);

#endif /* _PXE_STR_H */

