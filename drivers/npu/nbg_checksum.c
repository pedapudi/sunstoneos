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

/* The update of the reflected 0xedb88320 CRC (the CRC-32 of zlib and the Armv8 CRC32 instructions), with no inversion of its own.
 * On an A733 core (ID_AA64ISAR0_EL1.CRC32 is 1 on all eight, docs/hardware/a733-facts.json) the instructions take 8 bytes at a
 * time; the table-driven update takes one byte in about 14 cycles and verifying a graph reads it twice, which was 96 percent
 * of admission's time (783 ms of 816 for 13.8 MB at 494 MHz). The software update stays for every other target and is the
 * reference the host checks compare the instructions with. */
__attribute__((unused)) static uint32_t byte(uint32_t value, unsigned char data)
{
    value ^= data;
    value = table[value & 15] ^ (value >> 4);
    return table[value & 15] ^ (value >> 4);
}
#if defined(__aarch64__) && !defined(NBG_CRC_SOFTWARE)
#include <arm_acle.h>
__attribute__((target("crc"))) static uint32_t crc_update(uint32_t value, const unsigned char *data, size_t bytes)
{
    while (bytes && ((uintptr_t)data & 7)) { value = __crc32b(value, *data++); --bytes; }
    for (; bytes >= 8; data += 8, bytes -= 8) {
        /* The head loop made the pointer 8-byte aligned, so this is one aligned load. A memcpy of eight bytes here is built, in
         * an image that compiles for strict alignment, as eight byte loads and the shifts that join them: about 38 cycles a word
         * on an A55 instead of 3, which kept a layout at 135 ms that the instruction alone should take 25 ms for. */
        value = __crc32d(value, *(const uint64_t *)__builtin_assume_aligned(data, 8));
    }
    while (bytes--) value = __crc32b(value, *data++);
    return value;
}
#else
static uint32_t crc_update(uint32_t value, const unsigned char *data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) value = byte(value, data[i]);
    return value;
}
#endif

static uint32_t crc(const unsigned char *data, size_t bytes)
{
    return ~crc_update(UINT32_MAX, data, bytes);
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
    /* A listed section's own CRC is accumulated in the same walk: each chunk is fed to both CRCs while it is in the cache, so
     * the graph is read from memory once. A listed section no entry matches exactly is read again afterwards. */
    uint32_t part[NBG_SECTION_COUNT];
    uint64_t done = 0;
    for (unsigned i = 0; i < NBG_SECTION_COUNT; ++i) part[i] = UINT32_MAX;
    size_t end = start;
    for (uint32_t entry = 0; entry < word(data + 56); ++entry) {
        const unsigned char *p = data + 60 + entry * 44;
        size_t offset = word(p + 4), length = word(p + 12);
        if (!length) continue;
        if (offset < end) return NBG_CHECKSUM_INVALID;
        for (size_t i = end; i < offset; ++i)
            if (data[i]) return NBG_CHECKSUM_INVALID;
        /* The checksum section's own stored words count as zero; everything else is read as it is, in runs. */
        const size_t stop = offset + length, sum_begin = sums->offset, sum_end = sums->offset + sums->bytes;
        unsigned own = 0;
        for (uint32_t at = 0; at < sums->bytes; at += 8) {
            const unsigned kind = word(data + sums->offset + at);
            if (model.sections[kind - 1].offset == offset && model.sections[kind - 1].bytes == length &&
                kind != NBG_SECTION_CHECKSUM && !(done >> (kind - 1) & 1)) { own = kind; break; }
        }
        for (size_t i = offset; i < stop;) {
            if (i >= sum_begin && i < sum_end) {
                const unsigned char b = ((i - sum_begin) & 7) >= 4 ? 0 : data[i];
                value = crc_update(value, &b, 1);
                ++i;
            } else {
                const size_t next = i < sum_begin && sum_begin < stop ? sum_begin : stop;
                if (own) {
                    for (size_t chunk; i < next; i += chunk) {
                        chunk = next - i < 4096 ? next - i : 4096;
                        value = crc_update(value, data + i, chunk);
                        part[own - 1] = crc_update(part[own - 1], data + i, chunk);
                    }
                } else {
                    value = crc_update(value, data + i, next - i);
                    i = next;
                }
            }
        }
        if (own && !(sum_begin < stop && offset < sum_end)) done |= UINT64_C(1) << (own - 1);
        end = stop;
    }
    if (end != bytes) return NBG_CHECKSUM_INVALID;
    struct nbg_checksum check = {word(data + 12), ~value};
    if (check.stored == check.computed) {
        for (uint32_t at = 0; at < sums->bytes; at += 8) {
            unsigned kind = word(data + sums->offset + at);
            const struct nbg_section *s = &model.sections[kind - 1];
            struct nbg_checksum section = {word(data + sums->offset + at + 4),
                                          done >> (kind - 1) & 1 ? ~part[kind - 1] : crc(data + s->offset, s->bytes)};
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
    static const unsigned char zero[4];
    uint32_t value = crc_update(UINT32_MAX, data, 108);
    value = crc_update(value, zero, 4);
    value = crc_update(value, data + 112, bytes - 112);
    struct nbg_checksum check = {word(data + 108), ~value};
    *out = check;
    return check.stored == check.computed ? NBG_CHECKSUM_MATCH : NBG_CHECKSUM_MISMATCH;
}
