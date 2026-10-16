#include <string.h>

void *memcpy(void *restrict dest, const void *restrict src, size_t count)
{
    unsigned char *out = dest;
    const unsigned char *in = src;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
    return dest;
}

void *memset(void *dest, int value, size_t count)
{
    unsigned char *out = dest;
    for (size_t i = 0; i < count; ++i) out[i] = (unsigned char)value;
    return dest;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < count; ++i)
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}

size_t strlen(const char *text)
{
    size_t length = 0;
    while (text[length]) ++length;
    return length;
}
