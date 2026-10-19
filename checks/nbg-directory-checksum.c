#include "nbg_checksum.h"
#include "nbg_model.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned invalid, corruptions;
static uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}
/* Independent bitwise reference for repairing the primary checksum after a
 * mutation that must still fail a listed section checksum. */
static uint32_t primary(const unsigned char *data, size_t bytes)
{
    uint32_t value = UINT32_MAX;
    (void)bytes;
    for (unsigned entry = 0; entry < word(data + 56); ++entry) {
        size_t offset = word(data + 64 + entry * 44);
        size_t length = word(data + 72 + entry * 44);
        for (size_t i = offset; i < offset + length; ++i) {
            if (!((i >= 860 && i < 864) || (i >= 868 && i < 872) || (i >= 876 && i < 880)))
                value ^= data[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
        }
    }
    return ~value;
}
static void refuse(const unsigned char *data, size_t bytes)
{
    struct nbg_checksum out = {0x11223344,0x55667788}, saved = out;
    assert(nbg_checksum_verify(data, bytes, &out) == NBG_CHECKSUM_INVALID);
    assert(!memcmp(&out, &saved, sizeof(out))); ++invalid;
}
static void changed(unsigned char *data, size_t bytes, size_t offset)
{
    struct nbg_checksum out;
    data[offset] ^= 1;
    assert(nbg_checksum_verify(data, bytes, &out) != NBG_CHECKSUM_MATCH);
    data[offset] ^= 1; ++corruptions;
}
int main(int argc, char **argv)
{
    assert(argc == 4); /* Pinned convolution, synthetic gap, synthetic zero CRC. */
    for (unsigned fixture = 0; fixture < 3; ++fixture) {
        FILE *f = fopen(argv[fixture + 1], "rb"); assert(f);
        assert(!fseek(f, 0, SEEK_END)); long length = ftell(f); rewind(f);
        assert(length >= 1928 && length <= 4096); size_t bytes = (size_t)length;
        unsigned char *storage = malloc(bytes + 2), *copy = malloc(bytes); assert(storage && copy);
        unsigned char *data = storage + 1;
        assert(fread(data, 1, bytes, f) == bytes); fclose(f); memcpy(copy, data, bytes);
        storage[0] = storage[bytes + 1] = 0xa5;
        struct nbg_checksum out;
        assert(nbg_checksum_verify(data, bytes, &out) == NBG_CHECKSUM_MATCH);
        assert(out.stored == word(data + 12) && out.computed == primary(data, bytes));
        assert(!memcmp(data, copy, bytes));
        struct nbg_model decoded;
        assert(nbg_model_open(data, bytes, &decoded) == NBG_DECODE_OK);
        if (fixture == 1) { changed(data, bytes, 1600); changed(data, bytes, 1663); }
        if (fixture == 2) {
            assert(out.stored == 0 && out.computed == 0);
            changed(data, bytes, bytes - 1);
        }
        if (fixture == 0) {
            for (unsigned i = 0; i < 13; ++i) {
                unsigned at = 60 + i * 44;
                uint32_t offset = word(data + at + 4), size = word(data + at + 12);
                changed(data, bytes, offset); changed(data, bytes, offset + size - 1);
            }
            changed(data, bytes, 12);
            for (unsigned at = 860; at <= 876; at += 8) {
                put(data + at, 0);
                assert(nbg_checksum_verify(data, bytes, &out) == NBG_CHECKSUM_MISMATCH);
                assert(out.stored == 0 && out.computed == word(copy + at)); ++corruptions;
                memcpy(data, copy, bytes);
            }
            const unsigned payloads[] = {1244,1600,1856};
            for (unsigned i = 0; i < 3; ++i) {
                data[payloads[i]] ^= 1; put(data + 12, primary(data, bytes));
                assert(nbg_checksum_verify(data, bytes, &out) == NBG_CHECKSUM_MISMATCH);
                assert(out.stored == word(copy + 860 + i * 8) && out.stored != out.computed);
                ++corruptions; memcpy(data, copy, bytes);
            }
            const struct { unsigned at; uint32_t value; } bad[] = {
                {856,0},{856,40},{856,41},{856,6},{864,7},
                {60+2*44+12,0},{60+2*44+12,320},{60+2*44+32,4},
                {24,UINT32_MAX},{16,1927},{60+2*44+4,1920}
            };
            for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
                put(data + bad[i].at, bad[i].value); refuse(data, bytes); memcpy(data, copy, bytes);
            }
            /* A decoded but nonpacked metadata layout has no established CRC coverage. */
            put(data + 24, 620); memmove(data + 48, copy + 60, 13 * 44);
            memcpy(data + 620, copy + 48, 12); put(data + 620, 48);
            struct nbg_model view;
            assert(nbg_model_open(data, bytes, &view) == NBG_DECODE_OK);
            refuse(data, bytes); memcpy(data, copy, bytes);
            /* The primary checksum excludes these structurally valid header changes. */
            data[8] ^= 1;
            assert(nbg_checksum_verify(data, bytes, &out) == NBG_CHECKSUM_MATCH);
            memcpy(data, copy, bytes);
            struct nbg_checksum *alias = (void *)(storage + 8);
            unsigned char saved[8]; memcpy(saved, alias, 8);
            assert(nbg_checksum_verify(data, bytes, alias) == NBG_CHECKSUM_INVALID);
            assert(!memcmp(saved, alias, 8)); ++invalid;
        }
        assert(!memcmp(data, copy, bytes));
        assert(storage[0] == 0xa5 && storage[bytes + 1] == 0xa5);
        free(storage); free(copy);
    }
    printf("PASS directory checksums: pinned model, payload gap, valid zero CRC; %u corruptions, %u invalid cases\n", corruptions, invalid);
}
