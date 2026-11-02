#include "a733_storage_bounds.h"
#include "a733_storage_digest.h"
#include <string.h>

#define HEADER 48u

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const uint8_t *write_range(const struct a733_storage_bounds *b, uint32_t i) { return b->blob + HEADER + 16u * (size_t)i; }
static const uint8_t *backup_range(const struct a733_storage_bounds *b, uint32_t i) { return b->blob + HEADER + 16u * ((size_t)b->writes + i); }

/* Sorted, disjoint, nonempty, no wraparound. */
static int walk(const uint8_t *p, uint32_t n)
{
    uint64_t end = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t first = le64(p + 16u * (size_t)i), count = le64(p + 16u * (size_t)i + 8);
        if (!count || first < end || first > UINT64_MAX - count) return 0;
        end = first + count;
    }
    return 1;
}

int a733_storage_bounds_parse(struct a733_storage_bounds *b, const uint8_t *blob, size_t bytes)
{
    if (!b || !blob || b->state != A733_BOUNDS_EMPTY || bytes < HEADER || memcmp(blob, A733_BOUNDS_MAGIC, 8)) return 0;
    uint32_t w = le32(blob + 8), k = le32(blob + 12);
    if (!w || !k || (bytes - HEADER) % 16u || (bytes - HEADER) / 16u != (size_t)w + k) return 0;
    if (!walk(blob + HEADER, w) || !walk(blob + HEADER + 16u * (size_t)w, k)) return 0;
    b->blob = blob; b->writes = w; b->backups = k;
    b->state = A733_BOUNDS_PARSED;
    return 1;
}

int a733_storage_bounds_verify(struct a733_storage_bounds *b, int (*read)(void *, uint64_t, uint8_t *), void *context)
{
    if (!b || !read || b->state != A733_BOUNDS_PARSED) return 0;
    struct storage_digest d;
    storage_digest_init(&d);
    uint8_t record[520];
    int ok = 1;
    for (uint32_t i = 0; ok && i < b->backups; ++i) {
        uint64_t first = le64(backup_range(b, i)), count = le64(backup_range(b, i) + 8);
        for (uint64_t s = first; ok && s < first + count; ++s) {
            for (unsigned k = 0; k < 8; ++k) record[k] = (uint8_t)(s >> (8 * k));
            ok = read(context, s, record + 8) == 1 && storage_digest_add(&d, record, sizeof record) == 1;
        }
    }
    uint8_t digest[32];
    if (ok) storage_digest_end(&d, digest);
    /* One chance: a mismatch or a read failure closes the object for good. */
    if (!ok || memcmp(digest, b->blob + 16, 32)) { b->state = A733_BOUNDS_CLOSED; return 0; }
    b->state = A733_BOUNDS_VERIFIED;
    return 1;
}

int a733_storage_bounds_allows(const struct a733_storage_bounds *b, uint64_t first, uint64_t blocks)
{
    if (!b || b->state != A733_BOUNDS_VERIFIED || !blocks || first > UINT64_MAX - blocks) return 0;
    for (uint32_t i = 0; i < b->writes; ++i) {
        uint64_t f = le64(write_range(b, i)), c = le64(write_range(b, i) + 8);
        if (first >= f && first + blocks <= f + c) return 1;      /* f + c cannot wrap: parse checked it */
        if (f > first) break;                                      /* sorted: no later range can start earlier */
    }
    return 0;
}
