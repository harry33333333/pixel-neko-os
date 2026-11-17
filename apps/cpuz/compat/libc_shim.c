#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int printf(const char* format, ...) {
    (void)format;
    return 0;
}

int fprintf(FILE* stream, const char* format, ...) {
    (void)stream;
    (void)format;
    return 0;
}

char* getenv(const char* name) {
    (void)name;
    return NULL;
}

char* strstr(const char* haystack, const char* needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char*)haystack;
    for (const char* cursor = haystack; *cursor; cursor++) {
        const char* left = cursor;
        const char* right = needle;
        while (*left && *right && *left == *right) {
            left++;
            right++;
        }
        if (!*right) return (char*)cursor;
    }
    return NULL;
}

char* strdup(const char* text) {
    if (!text) return NULL;
    size_t length = strlen(text) + 1;
    char* copy = (char*)malloc(length);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    return copy;
}
