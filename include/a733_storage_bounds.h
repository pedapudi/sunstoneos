#ifndef SUNSTONE_A733_STORAGE_BOUNDS_H
#define SUNSTONE_A733_STORAGE_BOUNDS_H
/* The image side of the runner's bounded card-write admission (tools/storage_bounds.py). The runner delivers the
 * admitted bounds as the first input of the run; this object parses it, re-reads the backup sectors in order and
 * compares their hash with the admitted one, and from then on tells the card write path whether a CMD25 write of
 * `blocks` sectors at `first` lies wholly inside one admitted write range. Nothing is writable before the hash has
 * been verified, and a failed verification leaves the object permanently closed. */
#include <stddef.h>
#include <stdint.h>

#define A733_BOUNDS_MAGIC "SSBW0002"
#define A733_BOUNDS_MAX 32u            /* ranges of each kind */
struct a733_bounds_range { uint64_t first, count; };
enum a733_bounds_state { A733_BOUNDS_EMPTY, A733_BOUNDS_PARSED, A733_BOUNDS_VERIFIED, A733_BOUNDS_CLOSED };
struct a733_storage_bounds {
    enum a733_bounds_state state;
    unsigned writes, backups;
    uint8_t hash[32];
    struct a733_bounds_range write[A733_BOUNDS_MAX], backup[A733_BOUNDS_MAX];
};
/* Parse and check the blob: magic, counts, sorted disjoint nonempty ranges, length exactly. Returns 1. */
int a733_storage_bounds_parse(struct a733_storage_bounds *, const uint8_t *blob, size_t bytes);
/* Read every backup sector in order with `read` (one sector per call, 512 bytes), hash the records (u64 little-endian
 * sector, 512 bytes) and compare. Returns 1 and opens the write ranges only when the hash matches. */
int a733_storage_bounds_verify(struct a733_storage_bounds *, int (*read)(void *, uint64_t sector, uint8_t *out), void *context);
/* 1 when verified and [first, first + blocks) lies inside one write range. */
int a733_storage_bounds_allows(const struct a733_storage_bounds *, uint64_t first, uint64_t blocks);
/* The register-level rule for a data write: the exact CMD25 command word of the descriptor-DMA write (start, wait for
 * previous data, automatic stop, write, data, CRC, response, index 25), a 512-byte block size, a nonzero whole number
 * of blocks of at most 256, and a sector argument whose whole range is admitted. The caller reads ARG, BLKSZ and BCNT
 * from the controller at the moment of the command write. Sector addressing is required of the card elsewhere. */
#include "sunxi_smhc.h"
static inline int a733_storage_bounds_cmd25_ok(const struct a733_storage_bounds *b, uint32_t command, uint32_t argument,
                                               uint32_t block_size, uint32_t byte_count)
{
    const uint32_t exact = SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_WAIT | SUNXI_SMHC_CMD_AUTO_STOP | SUNXI_SMHC_CMD_WRITE |
                           SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_RESP | 25u;
    return command == exact && block_size == 512u && byte_count && !(byte_count % 512u) && byte_count <= 256u * 512u &&
           a733_storage_bounds_allows(b, argument, byte_count / 512u);
}
/* The write threshold register is writable by a verified bounded writer in every profile (the vendor sets it for each write). */
static inline int a733_storage_bounds_threshold_ok(const struct a733_storage_bounds *b)
{ return b && b->state == A733_BOUNDS_VERIFIED; }
#endif
