#include <stdint.h>
#include <string.h>

/* Same access rules as memcpy in memory.c: general registers only, bytes or
 * aligned 8-byte words, and no access outside either range. */
typedef uint64_t __attribute__((may_alias)) runtime_word;

__attribute__((target("+nofp"))) void *memmove(void *dest, const void *src, size_t count)
{
    unsigned char *out = dest;
    const unsigned char *in = src;
    int words = (((uintptr_t)out ^ (uintptr_t)in) & 7) == 0;
    if ((uintptr_t)out <= (uintptr_t)in) {
        if (words) {
            for (; count && ((uintptr_t)out & 7); --count) *out++ = *in++;
            for (; count >= 8; count -= 8, out += 8, in += 8)
                *(runtime_word *)out = *(const runtime_word *)in;
        }
        for (; count; --count) *out++ = *in++;
    } else {
        out += count;
        in += count;
        if (words) {
            for (; count && ((uintptr_t)out & 7); --count) *--out = *--in;
            for (; count >= 8; count -= 8) {
                out -= 8;
                in -= 8;
                *(runtime_word *)out = *(const runtime_word *)in;
            }
        }
        for (; count; --count) *--out = *--in;
    }
    return dest;
}
