/* The part of snprintf that the image's text records use: %s %c %d %i %u %x %X
 * and %%, with a zero flag, a width and the l and ll length modifiers. The
 * A733 image links newlib for the full function; the virtual board links no
 * C library. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

static void emit(char *out, size_t size, size_t *used, char c)
{
    if (*used + 1 < size) out[*used] = c;
    ++*used;
}
int snprintf(char *restrict out, size_t size, const char *restrict format, ...)
{
    va_list ap;
    va_start(ap, format);
    size_t used = 0;
    for (; *format; ++format) {
        if (*format != '%') { emit(out, size, &used, *format); continue; }
        ++format;
        int zero = 0;
        unsigned width = 0, longs = 0;
        if (*format == '0') { zero = 1; ++format; }
        while (*format >= '0' && *format <= '9') width = width * 10 + (unsigned)(*format++ - '0');
        while (*format == 'l') { ++longs; ++format; }
        char digits[24];
        unsigned count = 0;
        const char *text = 0;
        switch (*format) {
        case '%': emit(out, size, &used, '%'); continue;
        case 'c': emit(out, size, &used, (char)va_arg(ap, int)); continue;
        case 's': text = va_arg(ap, const char *); break;
        case 'd': case 'i': {
            int64_t v = longs ? va_arg(ap, long long) : va_arg(ap, int);
            uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
            do { digits[count++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) digits[count++] = '-';
            break;
        }
        case 'u': case 'x': case 'X': {
            uint64_t u = longs ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            const unsigned base = *format == 'u' ? 10 : 16;
            const char *set = *format == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do { digits[count++] = set[u % base]; u /= base; } while (u);
            break;
        }
        default: emit(out, size, &used, *format); continue;
        }
        if (text) { while (*text) emit(out, size, &used, *text++); continue; }
        for (unsigned pad = count; pad < width; ++pad) emit(out, size, &used, zero ? '0' : ' ');
        while (count) emit(out, size, &used, digits[--count]);
    }
    va_end(ap);
    if (size) out[used < size ? used : size - 1] = 0;
    return (int)used;
}
