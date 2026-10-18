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

int strcmp(const char *left, const char *right)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    while (*a && *a == *b) { ++a; ++b; }
    return (int)*a - (int)*b;
}

char *strchr(const char *text, int value)
{
    unsigned char match = (unsigned char)value;
    for (;;) {
        if ((unsigned char)*text == match) return (char *)text;
        if (!*text) return NULL;
        ++text;
    }
}

char *strcpy(char *restrict destination, const char *restrict source)
{
    char *result = destination;
    do { *destination++ = *source; } while (*source++);
    return result;
}

char *strncpy(char *restrict destination, const char *restrict source, size_t count)
{
    size_t i = 0;
    for (; i < count && source[i]; ++i) destination[i] = source[i];
    for (; i < count; ++i) destination[i] = 0;
    return destination;
}

int strcasecmp(const char *left, const char *right)
{
    for (;;) {
        unsigned char a = (unsigned char)*left++, b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b || !a) return (int)a - (int)b;
    }
}
