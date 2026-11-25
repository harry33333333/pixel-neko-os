#ifndef _VPLAYER_COMPAT_STRING_H
#define _VPLAYER_COMPAT_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pxe/str.h>

char* strstr(const char* haystack, const char* needle);
char* strdup(const char* s);

#ifdef __cplusplus
}
#endif

#endif
