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

static uint32_t byte(uint32_t value, unsigned char data)
{
    value ^= data;
    value = table[value & 15] ^ (value >> 4);
    return table[value & 15] ^ (value >> 4);
}

static uint32_t crc(const unsigned char *data, size_t bytes)
{
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < bytes; ++i) value = byte(value, data[i]);
    return ~value;
}

static enum nbg_checksum_result directory(const unsigned char *data, size_t bytes,
                                           struct nbg_checksum *out)
{
    struct nbg_model model;
    if (nbg_model_open(data, bytes, &model) != NBG_DECODE_OK)
        return NBG_CHECKSUM_INVALID;
    /* The inspected writer emits header, subheader, and directory consecutively.
     * Other placements can be decoded, but their CRC coverage is not established. */
    if (word(data + 24) != 48 || word(data + 48) != 60)
        return NBG_CHECKSUM_INVALID;
    size_t start = 60 + (size_t)word(data + 56) * 44;
    for (unsigned i = 0; i < NBG_SECTION_COUNT; ++i)
        if (model.sections[i].bytes && model.sections[i].offset < start)
            return NBG_CHECKSUM_INVALID;
    const struct nbg_section *sums = &model.sections[NBG_SECTION_CHECKSUM];
    if (!sums->bytes || sums->bytes > (NBG_SECTION_COUNT - 1) * 8)
        return NBG_CHECKSUM_INVALID;
    uint64_t seen = 0;
    for (uint32_t at = 0; at < sums->bytes; at += 8) {
        unsigned kind = word(data + sums->offset + at);
        if (!kind || kind >= NBG_SECTION_COUNT || !model.sections[kind - 1].bytes)
            return NBG_CHECKSUM_INVALID;
        uint64_t bit = UINT64_C(1) << (kind - 1);
        if (seen & bit) return NBG_CHECKSUM_INVALID;
        seen |= bit;
    }
    /* SaveSectionBuffer in the pinned writer seeks to each section offset and
     * accumulates only its declared payload. Alignment gaps are excluded. Keep
     * the admitted layout canonical: ordered payloads, zero gaps, no trailer.
     * nbg_model_open separately bounds every section and section-relative table. */
    uint32_t value = UINT32_MAX;
    size_t end = start;
    for (uint32_t entry = 0; entry < word(data + 56); ++entry) {
        const unsigned char *p = data + 60 + entry * 44;
        size_t offset = word(p + 4), length = word(p + 12);
        if (!length) continue;
        if (offset < end) return NBG_CHECKSUM_INVALID;
        for (size_t i = end; i < offset; ++i)
            if (data[i]) return NBG_CHECKSUM_INVALID;
        for (size_t i = offset; i < offset + length; ++i) {
            int normalized = i >= sums->offset && i - sums->offset < sums->bytes &&
                             ((i - sums->offset) & 7) >= 4;
            value = byte(value, normalized ? 0 : data[i]);
        }
        end = offset + length;
    }
    if (end != bytes) return NBG_CHECKSUM_INVALID;
    struct nbg_checksum check = {word(data + 12), ~value};
    if (check.stored == check.computed) {
        for (uint32_t at = 0; at < sums->bytes; at += 8) {
            unsigned kind = word(data + sums->offset + at);
            const struct nbg_section *s = &model.sections[kind - 1];
            struct nbg_checksum section = {word(data + sums->offset + at + 4),
                                          crc(data + s->offset, s->bytes)};
            if (section.stored != section.computed) {
                *out = section;
                return NBG_CHECKSUM_MISMATCH;
            }
        }
    }
    *out = check;
    return check.stored == check.computed ? NBG_CHECKSUM_MATCH : NBG_CHECKSUM_MISMATCH;
}

enum nbg_checksum_result nbg_checksum_verify(const void *input, size_t bytes,
                                             struct nbg_checksum *out)
{
    uintptr_t begin = (uintptr_t)input, result = (uintptr_t)out;
    if (!input || !out || bytes < NBG_MODEL_DIRECTORY_HEADER_BYTES || bytes > NBG_MODEL_MAX_BYTES ||
        bytes > UINTPTR_MAX - begin || sizeof(*out) > UINTPTR_MAX - result ||
        (begin < result + sizeof(*out) && result < begin + bytes)) return NBG_CHECKSUM_INVALID;
    const unsigned char *data = input;
    if (word(data) != 0x4e4d5056) return NBG_CHECKSUM_INVALID;
    if (word(data + 4) == NBG_MODEL_DIRECTORY_VERSION) return directory(data, bytes, out);
    if (word(data + 4) != NBG_MODEL_VERSION || bytes < NBG_MODEL_HEADER_BYTES)
        return NBG_CHECKSUM_INVALID;
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < bytes; ++i) {
        value = byte(value, i < 108 || i >= 112 ? data[i] : 0);
    }
    struct nbg_checksum check = {word(data + 108), ~value};
    *out = check;
    return check.stored == check.computed ? NBG_CHECKSUM_MATCH : NBG_CHECKSUM_MISMATCH;
}
