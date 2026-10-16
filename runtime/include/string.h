#ifndef SUNSTONE_STRING_H
#define SUNSTONE_STRING_H
#include <stddef.h>
void *memcpy(void *restrict dest, const void *restrict src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
size_t strlen(const char *text);
#endif
