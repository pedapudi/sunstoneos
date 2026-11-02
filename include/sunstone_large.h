#ifndef SUNSTONE_LARGE_H
#define SUNSTONE_LARGE_H
/* Large application regions (docs/designs/large-app-regions.md): the record an application declares
 * in the optional section "sunstone_large_regions", and the status the SDK reports. The record is
 * little-endian data of 112 bytes; the section holds an array of them, aligned to 8 bytes. */
#include <stddef.h>
#include <stdint.h>

#define SS_LARGE_REGIONS 4u
#define SS_LARGE_PATH_BYTES 48u
enum {
    SS_LARGE_WRITE = 1u << 0,    /* the application may write the region at EL0 */
    SS_LARGE_EXTENT = 1u << 1    /* the source is a raw card extent (first_sector), not a file */
};
struct ss_large_region {
    uint32_t index, flags;
    uint64_t bytes;              /* capacity: a multiple of 2 MiB */
    uint64_t source_bytes;       /* exact length of the source; at most bytes */
    uint64_t first_sector;       /* raw extent only */
    uint8_t sha256[32];          /* digest of the source bytes; never all zero */
    char path[SS_LARGE_PATH_BYTES];   /* file path from the root, NUL-padded; empty for an extent */
};
_Static_assert(sizeof(struct ss_large_region) == 112 && offsetof(struct ss_large_region, bytes) == 8 &&
               offsetof(struct ss_large_region, sha256) == 32 && offsetof(struct ss_large_region, path) == 64,
               "ss_large_region layout");

enum { SS_LARGE_PENDING = 0, SS_LARGE_LOADING = 1, SS_LARGE_READY = 2, SS_LARGE_FAILED = 3 };
struct ss_large_status {
    uint32_t state, error;       /* error: 0 or a loader status (negative values are stored as unsigned) */
    uint64_t loaded, total, elapsed_us;
};

/* Validate the records of one application: at most SS_LARGE_REGIONS, indexes 0..count-1 each once, sizes
 * and digests, path characters and the source kind. 1 when every record is well formed. */
int ss_large_records_valid(const struct ss_large_region *records, unsigned count);
#endif
