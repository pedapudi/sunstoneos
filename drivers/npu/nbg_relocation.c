#include "nbg_relocation.h"

static int cpu_range(const void *p, size_t n)
{
    return p != NULL && n != 0 && n <= UINTPTR_MAX - (uintptr_t)p;
}

static uint32_t read_word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write_word(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i != 4; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

int nbg_relocate(void *buffer, size_t bytes,
                 const struct nbg_relocation *patches, size_t count)
{
    const uint64_t address_space = UINT64_C(1) << 32;
    if (!count || count > NBG_RELOCATION_MAX_PATCHES ||
        !cpu_range(buffer, bytes) ||
        !cpu_range(patches, count * sizeof(*patches)))
        return 0;
    uintptr_t begin = (uintptr_t)buffer, meta = (uintptr_t)patches;
    if (begin < meta + count * sizeof(*patches) && meta < begin + bytes)
        return 0;

    /* Validate the entire batch before modifying any destination. */
    for (size_t i = 0; i != count; ++i) {
        const struct nbg_relocation *p = &patches[i];
        if (!p->source_device || !p->source_bytes ||
            p->source_bytes > address_space - p->source_device ||
            !p->referenced_bytes || p->source_offset >= p->source_bytes ||
            p->referenced_bytes > p->source_bytes - p->source_offset ||
            (p->destination_offset & 3) ||
            p->destination_offset > bytes || bytes - p->destination_offset < 4 ||
            (p->encoding != NBG_ADDRESS_WORD && p->encoding != NBG_ADDRESS_SHIFT6 &&
             p->encoding != NBG_ADDRESS_FLAGS6))
            return 0;
        uint32_t address = p->source_device + p->source_offset;
        if (p->encoding != NBG_ADDRESS_WORD && (address & 63))
            return 0;
        for (size_t j = 0; j != i; ++j)
            if (patches[j].destination_offset == p->destination_offset)
                return 0;
    }
    for (size_t i = 0; i != count; ++i) {
        const struct nbg_relocation *p = &patches[i];
        unsigned char *dst = (unsigned char *)buffer + p->destination_offset;
        uint32_t address = p->source_device + p->source_offset;
        uint32_t value = address;
        if (p->encoding == NBG_ADDRESS_SHIFT6)
            value = (read_word(dst) & UINT32_C(0xfc000000)) | (address >> 6);
        else if (p->encoding == NBG_ADDRESS_FLAGS6)
            value = address | (read_word(dst) & 63);
        write_word(dst, value);
    }
    return 1;
}
