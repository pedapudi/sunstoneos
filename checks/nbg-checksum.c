#include "nbg_checksum.h"
#include "nbg_model.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned corruptions, invalid;
static uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void refuse(const void *data, size_t bytes, struct nbg_checksum *out)
{
    unsigned char saved[sizeof(*out)]; memcpy(saved, out, sizeof(*out));
    assert(nbg_checksum_verify(data, bytes, out) == NBG_CHECKSUM_INVALID);
    assert(!memcmp(saved, out, sizeof(*out))); ++invalid;
}
static void changed(unsigned char *data, size_t bytes, size_t offset)
{
    struct nbg_checksum out;
    data[offset] ^= 1;
    assert(nbg_checksum_verify(data, bytes, &out) == NBG_CHECKSUM_MISMATCH);
    assert(out.stored != out.computed);
    data[offset] ^= 1; ++corruptions;
}
int main(int argc, char **argv)
{
    assert(argc == 4); /* Two pinned models and a reference-generated zero-CRC file. */
    const uint32_t sums[] = {0xdd476ad1, 0x48739b09, 0};
    for (unsigned fixture = 0; fixture < 3; ++fixture) {
        FILE *f = fopen(argv[fixture + 1], "rb"); assert(f);
        assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); assert(size >= 592); rewind(f);
        size_t bytes = (size_t)size;
        unsigned char *allocation = malloc(bytes + 2); assert(allocation);
        unsigned char *data = allocation + 1; /* Unaligned input with byte guards. */
        allocation[0] = allocation[bytes + 1] = 0xa5;
        assert(fread(data, 1, bytes, f) == bytes); fclose(f);
        struct nbg_checksum out;
        assert(nbg_checksum_verify(data, bytes, &out) == NBG_CHECKSUM_MATCH);
        assert(out.stored == sums[fixture] && out.computed == sums[fixture]);
        if (fixture == 2) {
            /* A valid zero checksum is not an absent or disabled checksum. */
            changed(data, bytes, bytes - 1);
            free(allocation); continue;
        }
        assert(word(data + 108) == sums[fixture]);
        changed(data, bytes, 8); changed(data, bytes, bytes - 1);
        for (unsigned i = 108; i < 112; ++i) changed(data, bytes, i);
        for (unsigned i = 0; i < NBG_MODEL_FIXED_SECTION_COUNT; ++i) {
            uint32_t start = word(data + 376 + i * 8), length = word(data + 380 + i * 8);
            assert(start <= bytes && length <= bytes - start);
            if (length) {
                changed(data, bytes, start);
                changed(data, bytes, start + length - 1);
            }
        }
        if (fixture == 0) changed(data, bytes, 63356); /* Gap between tables and coefficients. */
        unsigned char checksum[4]; memcpy(checksum, data + 108, 4);
        memset(data + 108, 0, 4);
        assert(nbg_checksum_verify(data, bytes, &out) == NBG_CHECKSUM_MISMATCH);
        assert(out.stored == 0 && out.computed == sums[fixture]); ++corruptions;
        memcpy(data + 108, checksum, 4);
        refuse(NULL, bytes, &out); refuse(data, 0, &out); refuse(data, 591, &out);
        refuse(data, (size_t)NBG_MODEL_MAX_BYTES + 1, &out);
        refuse((void *)(UINTPTR_MAX - 3), 592, &out);
        assert(nbg_checksum_verify(data, bytes, NULL) == NBG_CHECKSUM_INVALID); ++invalid;
        unsigned char *aligned = (unsigned char *)(((uintptr_t)data + 7) & ~(uintptr_t)7);
        refuse(data, bytes, (struct nbg_checksum *)aligned);
        data[0] ^= 1; refuse(data, bytes, &out); data[0] ^= 1;
        data[4] ^= 1; refuse(data, bytes, &out); data[4] ^= 1;
        assert(allocation[0] == 0xa5 && allocation[bytes + 1] == 0xa5);
        free(allocation);
    }
    printf("PASS checksum: two pinned models, valid zero CRC, %u corruptions, %u invalid-input cases\n",
           corruptions, invalid);
}
