#include <stdint.h>
#include <string.h>

void *memmove(void *dest, const void *src, size_t count)
{
    unsigned char *out = dest;
    const unsigned char *in = src;
    if ((uintptr_t)out <= (uintptr_t)in) {
        for (size_t i = 0; i < count; ++i) out[i] = in[i];
    } else {
        while (count) { --count; out[count] = in[count]; }
    }
    return dest;
}
