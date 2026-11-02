/* Host check of the image side of the bounded-write admission (drivers/storage/a733_storage_bounds.c) against blobs
 * and backup files produced by tools/storage_bounds.py. Usage: storage-bounds-image BLOB CARD FIRST_BAD
 * BLOB is the runner's input, CARD a file of sectors (sector n at offset 512 n). */
#define _FILE_OFFSET_BITS 64
#include "a733_storage_bounds.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *card; static unsigned reads;
static uint64_t le64(const uint8_t *p) { uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i); return v; }
static uint64_t wfirst(const struct a733_storage_bounds *b, unsigned i) { return le64(b->blob + 48 + 16 * (size_t)i); }
static uint64_t wcount(const struct a733_storage_bounds *b, unsigned i) { return le64(b->blob + 48 + 16 * (size_t)i + 8); }
static uint64_t bfirst(const struct a733_storage_bounds *b, unsigned i) { return le64(b->blob + 48 + 16 * ((size_t)b->writes + i)); }
static uint64_t bcount(const struct a733_storage_bounds *b, unsigned i) { return le64(b->blob + 48 + 16 * ((size_t)b->writes + i) + 8); }
static int read_sector(void *c, uint64_t sector, uint8_t *out)
{
    (void)c; ++reads;
    if (fseeko(card, (off_t)(sector * 512), SEEK_SET)) return 0;
    return fread(out, 1, 512, card) == 512;
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    static uint8_t blob[65536]; size_t n = fread(blob, 1, sizeof blob, f); fclose(f);
    card = fopen(argv[2], "r+b"); assert(card);
    struct a733_storage_bounds b = {0};
    /* Nothing is writable before parse and verify. */
    assert(!a733_storage_bounds_allows(&b, 100, 1) && !a733_storage_bounds_verify(&b, read_sector, NULL));
    /* Malformed blobs. */
    static uint8_t bad[65536];
    for (unsigned i = 0; i < 8; ++i) {
        memcpy(bad, blob, n); size_t bn = n;
        switch (i) {
        case 0: bad[0] ^= 1; break;                                  /* magic */
        case 1: bn = n - 1; break;                                   /* length */
        case 2: bn = n + 1; break;
        case 3: bad[8] = 0; bad[9] = bad[10] = bad[11] = 0; break;   /* no write ranges */
        case 4: bad[8] = 200; break;                                 /* count longer than the blob */
        case 5: memset(bad + 48 + 8, 0, 8); break;                   /* empty first range */
        case 6: { uint8_t t[16]; memcpy(t, bad + 48, 16); memcpy(bad + 48, bad + 48 + 16, 16); memcpy(bad + 48 + 16, t, 16); } break;   /* unsorted */
        case 7: bad[12] = 0; bad[13] = bad[14] = bad[15] = 0; break; /* no backup ranges */
        }
        struct a733_storage_bounds x = {0};
        assert(!a733_storage_bounds_parse(&x, bad, bn) && x.state == A733_BOUNDS_EMPTY);
    }
    assert(a733_storage_bounds_parse(&b, blob, n) && b.state == A733_BOUNDS_PARSED);
    assert(!a733_storage_bounds_allows(&b, wfirst(&b, 0), 1));      /* parsed but not verified */
    unsigned backup_sectors = 0; for (unsigned i = 0; i < b.backups; ++i) backup_sectors += (unsigned)bcount(&b, i);
    assert(a733_storage_bounds_verify(&b, read_sector, NULL) && b.state == A733_BOUNDS_VERIFIED && reads == backup_sectors);
    /* Boundaries of every write range. */
    for (unsigned i = 0; i < b.writes; ++i) {
        uint64_t f0 = wfirst(&b, i), c = wcount(&b, i);
        assert(a733_storage_bounds_allows(&b, f0, 1) && a733_storage_bounds_allows(&b, f0, c) && a733_storage_bounds_allows(&b, f0 + c - 1, 1));
        int adjacent = i + 1 < b.writes && wfirst(&b, i + 1) == f0 + c;
        assert(!a733_storage_bounds_allows(&b, f0, c + 1) && !a733_storage_bounds_allows(&b, f0 + c - 1, 2));   /* no write spans two ranges */
        assert(adjacent || !a733_storage_bounds_allows(&b, f0 + c, 1));
        assert(!a733_storage_bounds_allows(&b, f0, 0) && !a733_storage_bounds_allows(&b, UINT64_MAX, 2));
        assert(f0 == 0 || !a733_storage_bounds_allows(&b, f0 - 1, 2));
    }
    /* The register-level rule for CMD25: the exact word, 512-byte blocks, at most 256 of them, an admitted range. */
    {
        const uint32_t cmd = (1u << 31) | (1u << 13) | (1u << 12) | (1u << 10) | (1u << 9) | (1u << 8) | (1u << 6) | 25u;
        uint64_t f0 = wfirst(&b, b.writes - 1), c = wcount(&b, b.writes - 1);
        uint32_t blocks = c < 256 ? (uint32_t)c : 256;
        assert(a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)f0, 512, blocks * 512));
        assert(a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)(f0 + c - 1), 512, 512));
        assert(!a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)(f0 + c - 1), 512, 1024));          /* runs past the range */
        assert(!a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)(f0 - 1), 512, 512));
        assert(!a733_storage_bounds_cmd25_ok(&b, cmd ^ (1u << 12), (uint32_t)f0, 512, 512));       /* no automatic stop */
        assert(!a733_storage_bounds_cmd25_ok(&b, cmd ^ (1u << 10), (uint32_t)f0, 512, 512));       /* a read */
        assert(!a733_storage_bounds_cmd25_ok(&b, (cmd & ~63u) | 24u, (uint32_t)f0, 512, 512));     /* CMD24 */
        assert(!a733_storage_bounds_cmd25_ok(&b, cmd | (1u << 7), (uint32_t)f0, 512, 512));
        assert(!a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)f0, 256, 512) && !a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)f0, 1024, 512));
        assert(!a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)f0, 512, 0) && !a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)f0, 512, 513));
        assert(!a733_storage_bounds_cmd25_ok(&b, cmd, (uint32_t)f0, 512, 257 * 512));
        assert(a733_storage_bounds_threshold_ok(&b) && !a733_storage_bounds_threshold_ok(NULL));
    }
    /* The blob must not change after parse: with every kernel byte writable at EL0, each use re-hashes it. */
    {
        static uint8_t copy[65536];
        memcpy(copy, blob, n);
        struct a733_storage_bounds t = {0};
        assert(a733_storage_bounds_parse(&t, copy, n) && a733_storage_bounds_verify(&t, read_sector, NULL));
        uint64_t f0 = wfirst(&t, 0);
        assert(a733_storage_bounds_allows(&t, f0, 1) && a733_storage_bounds_intact(&t) && a733_storage_bounds_threshold_ok(&t));
        for (size_t at = 0; at < n; at += (at < 64 ? 1 : 97)) {
            copy[at] ^= 0x01;                                         /* any single changed bit, in the header or the ranges */
            assert(!a733_storage_bounds_intact(&t) && !a733_storage_bounds_allows(&t, f0, 1) && !a733_storage_bounds_threshold_ok(&t));
            copy[at] ^= 0x01;
            assert(a733_storage_bounds_allows(&t, f0, 1));
        }
        struct a733_storage_bounds u = {0};
        memcpy(copy, blob, n);
        assert(a733_storage_bounds_parse(&u, copy, n));
        copy[20] ^= 0x80;                                             /* changed between parse and verify */
        assert(!a733_storage_bounds_verify(&u, read_sector, NULL));
    }
    assert(!a733_storage_bounds_allows(&b, 0, 1));
    assert(!a733_storage_bounds_parse(&b, blob, n));                   /* one parse per object */
    /* A card that changed since the backup closes the object for good. */
    struct a733_storage_bounds c = {0};
    assert(a733_storage_bounds_parse(&c, blob, n));
    uint8_t byte; uint64_t s = bfirst(&c, c.backups - 1);
    fseeko(card, (off_t)(s * 512 + 100), SEEK_SET); assert(fread(&byte, 1, 1, card) == 1);
    byte ^= 0x40; fseeko(card, (off_t)(s * 512 + 100), SEEK_SET); fwrite(&byte, 1, 1, card); fflush(card);
    reads = 0;
    assert(!a733_storage_bounds_verify(&c, read_sector, NULL) && c.state == A733_BOUNDS_CLOSED);
    assert(!a733_storage_bounds_allows(&c, wfirst(&c, 0), 1) && !a733_storage_bounds_verify(&c, read_sector, NULL));
    assert(!a733_storage_bounds_cmd25_ok(&c, (1u << 31) | (1u << 13) | (1u << 12) | (1u << 10) | (1u << 9) | (1u << 8) | (1u << 6) | 25u,
                                         (uint32_t)wfirst(&c, 0), 512, 512) && !a733_storage_bounds_threshold_ok(&c));
    /* A read failure closes it as well. */
    struct a733_storage_bounds d = {0};
    assert(a733_storage_bounds_parse(&d, blob, n));
    fclose(card); card = fopen(argv[2], "rb"); assert(card);
    assert(!a733_storage_bounds_verify(&d, read_sector, NULL) && d.state == A733_BOUNDS_CLOSED);     /* the changed byte also fails here */
    struct a733_storage_bounds e = {0};
    assert(a733_storage_bounds_parse(&e, blob, n));
    fclose(card); card = fopen("/dev/null", "rb"); assert(card);
    assert(!a733_storage_bounds_verify(&e, read_sector, NULL) && e.state == A733_BOUNDS_CLOSED);
    printf("ranges: %u write, %u backup\n", b.writes, b.backups);
    puts("PASS: bounds blob parse (8 malformed forms), hash re-read of the listed sectors in order, range boundaries, one parse per object, closed after a changed card or a read failure");
    return 0;
}
