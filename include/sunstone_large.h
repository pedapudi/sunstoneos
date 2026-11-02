#ifndef SUNSTONE_LARGE_H
#define SUNSTONE_LARGE_H
/* Large application regions (docs/designs/large-app-regions.md): the record an application declares
 * in the optional section "sunstone_large_regions", and the status the SDK reports. The record is
 * little-endian data of 112 bytes; the section holds an array of them in index order, aligned to 8 bytes.
 * An application declares as many as it needs; the bound is the upper memory itself (each region takes at
 * least one 2 MiB block, and the regions of one application together fit in it). */
#include <stddef.h>
#include <stdint.h>

#define SS_LARGE_PATH_BYTES 48u
enum {
    SS_LARGE_WRITE = 1u << 0,    /* the application may write the region at EL0 */
    SS_LARGE_EXTENT = 1u << 1,   /* the source is a raw card extent (first_sector), not a file */
    SS_LARGE_UNFILLED = 1u << 2, /* no source: the region is READY at once and its contents are whatever the memory held;
                                  * source_bytes, first_sector, the digest and the path are all zero */
    SS_LARGE_HEAP = 1u << 3      /* with UNFILLED and WRITE: the application's C and C++ heap, the last record; the C library
                                  * takes it and zeroes what it hands out */
};
struct ss_large_region {
    uint32_t index, flags;
    uint64_t bytes;              /* capacity: a multiple of 2 MiB */
    uint64_t source_bytes;       /* exact length of the source; at most bytes; zero for the heap */
    uint64_t first_sector;       /* raw extent only */
    uint8_t sha256[32];          /* digest of the source bytes; all zero: the loader does not hash and READY follows the last byte */
    char path[SS_LARGE_PATH_BYTES];   /* file path from the root, NUL-padded; empty for an extent */
};
_Static_assert(sizeof(struct ss_large_region) == 112 && offsetof(struct ss_large_region, bytes) == 8 &&
               offsetof(struct ss_large_region, sha256) == 32 && offsetof(struct ss_large_region, path) == 64,
               "ss_large_region layout");

enum { SS_LARGE_PENDING = 0, SS_LARGE_LOADING = 1, SS_LARGE_READY = 2, SS_LARGE_FAILED = 3 };
struct ss_large_status {
    uint32_t state, error;       /* error: 0 or a loader status (negative values are stored as unsigned) */
    uint64_t loaded, total, elapsed_us;
    uint64_t copy_us, digest_us; /* of elapsed_us, the loader's copy into the region and its SHA-256; the rest is reading */
};

/* Validate the records of one application: record i has index i, sizes that are 2 MiB multiples whose total
 * fits the upper memory, a digest, path characters and one source kind. 1 when every record is well formed. */
int ss_large_records_valid(const struct ss_large_region *records, unsigned count);
#endif
