#include "a733_storage_bounds.h"
#include "a733_storage_digest.h"
#include <string.h>

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int ranges(struct a733_bounds_range *out, unsigned n, const uint8_t *p)
{
    uint64_t end = 0;
    for (unsigned i = 0; i < n; ++i) {
        uint64_t first = le64(p + 16 * i), count = le64(p + 16 * i + 8);
        if (!count || first < end || first > UINT64_MAX - count) return 0;
        out[i] = (struct a733_bounds_range){first, count};
        end = first + count;
    }
    return 1;
}

int a733_storage_bounds_parse(struct a733_storage_bounds *b, const uint8_t *blob, size_t bytes)
{
    if (!b || !blob || b->state != A733_BOUNDS_EMPTY || bytes < 8 + 8 + 32 || memcmp(blob, A733_BOUNDS_MAGIC, 8)) return 0;
    uint32_t w = le32(blob + 8), k = le32(blob + 12);
    if (!w || !k || w > A733_BOUNDS_MAX || k > A733_BOUNDS_MAX || bytes != 48u + 16u * ((size_t)w + k)) return 0;
    struct a733_storage_bounds fresh = {0};
    fresh.writes = w; fresh.backups = k;
    memcpy(fresh.hash, blob + 16, 32);
    /* Backup ranges may begin at sector 0 (the MBR); both lists are sorted, disjoint and nonempty. */
    if (!ranges(fresh.write, w, blob + 48) || !ranges(fresh.backup, k, blob + 48 + 16u * w)) return 0;
    fresh.state = A733_BOUNDS_PARSED;
    *b = fresh;
    return 1;
}

int a733_storage_bounds_verify(struct a733_storage_bounds *b, int (*read)(void *, uint64_t, uint8_t *), void *context)
{
    if (!b || !read || b->state != A733_BOUNDS_PARSED) return 0;
    struct storage_digest d;
    storage_digest_init(&d);
    uint8_t record[520];
    int ok = 1;
    for (unsigned i = 0; ok && i < b->backups; ++i)
        for (uint64_t s = b->backup[i].first; ok && s < b->backup[i].first + b->backup[i].count; ++s) {
            for (unsigned k = 0; k < 8; ++k) record[k] = (uint8_t)(s >> (8 * k));
            ok = read(context, s, record + 8) == 1 && storage_digest_add(&d, record, sizeof record) == 1;
        }
    uint8_t digest[32];
    if (ok) storage_digest_end(&d, digest);
    /* One chance: a mismatch or a read failure closes the object for good. */
    if (!ok || memcmp(digest, b->hash, 32)) { b->state = A733_BOUNDS_CLOSED; return 0; }
    b->state = A733_BOUNDS_VERIFIED;
    return 1;
}

int a733_storage_bounds_allows(const struct a733_storage_bounds *b, uint64_t first, uint64_t blocks)
{
    if (!b || b->state != A733_BOUNDS_VERIFIED || !blocks || first > UINT64_MAX - blocks) return 0;
    for (unsigned i = 0; i < b->writes; ++i)
        if (first >= b->write[i].first && first + blocks <= b->write[i].first + b->write[i].count) return 1;
    return 0;
}
