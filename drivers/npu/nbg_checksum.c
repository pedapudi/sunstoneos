#include "nbg_checksum.h"
#include "nbg_model.h"

/* Reflected 0xedb88320 polynomial; two four-bit steps per byte avoid a mutable
 * table initializer and reserve only 64 bytes of read-only table storage. */
static const uint32_t table[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
};
static uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

enum nbg_checksum_result nbg_checksum_verify(const void *input, size_t bytes,
                                             struct nbg_checksum *out)
{
    uintptr_t begin = (uintptr_t)input, result = (uintptr_t)out;
    if (!input || !out || bytes < NBG_MODEL_HEADER_BYTES || bytes > NBG_MODEL_MAX_BYTES ||
        bytes > UINTPTR_MAX - begin || sizeof(*out) > UINTPTR_MAX - result ||
        (begin < result + sizeof(*out) && result < begin + bytes)) return NBG_CHECKSUM_INVALID;
    const unsigned char *data = input;
    if (word(data) != 0x4e4d5056 || word(data + 4) != NBG_MODEL_VERSION)
        return NBG_CHECKSUM_INVALID;
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < bytes; ++i) {
        if (i < 108 || i >= 112) value ^= data[i];
        value = table[value & 15] ^ (value >> 4);
        value = table[value & 15] ^ (value >> 4);
    }
    struct nbg_checksum check = {word(data + 108), ~value};
    *out = check;
    return check.stored == check.computed ? NBG_CHECKSUM_MATCH : NBG_CHECKSUM_MISMATCH;
}
