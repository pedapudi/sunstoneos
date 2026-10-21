#include <stdint.h>
#include <string.h>

/* Boot C calls these at EL3 with translation off, where every access is
 * Device-nGnRnE and must be naturally aligned. Only general registers are used
 * and each access is a byte or an aligned 8-byte word inside the caller's
 * ranges, so the routines are valid on Device and Normal memory alike. The
 * target attribute keeps SIMD out where the image does not pass
 * -mgeneral-regs-only; -fno-builtin keeps the loops from becoming calls. */
#define RUNTIME_GENERAL_REGS __attribute__((target("+nofp")))
typedef uint64_t __attribute__((may_alias)) runtime_word;

RUNTIME_GENERAL_REGS void *memcpy(void *restrict dest, const void *restrict src, size_t count)
{
    unsigned char *out = dest;
    const unsigned char *in = src;
    if ((((uintptr_t)out ^ (uintptr_t)in) & 7) == 0) {
        for (; count && ((uintptr_t)out & 7); --count) *out++ = *in++;
        for (; count >= 8; count -= 8, out += 8, in += 8)
            *(runtime_word *)out = *(const runtime_word *)in;
    }
    for (; count; --count) *out++ = *in++;
    return dest;
}

RUNTIME_GENERAL_REGS void *memset(void *dest, int value, size_t count)
{
    unsigned char *out = dest, byte = (unsigned char)value;
    for (; count && ((uintptr_t)out & 7); --count) *out++ = byte;
    runtime_word word = byte * UINT64_C(0x0101010101010101);
    for (; count >= 8; count -= 8, out += 8) *(runtime_word *)out = word;
    for (; count; --count) *out++ = byte;
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
