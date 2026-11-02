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
/* No copy of the ranges is made: the object keeps a pointer to the blob the runner delivered as the first input and
 * walks the ranges from there, so the only limit is the size of that input. The image keeps the input read-only for as
 * long as the object is used. Ranges are little-endian (first sector, sector count) u64 pairs. */
enum a733_bounds_state { A733_BOUNDS_EMPTY, A733_BOUNDS_PARSED, A733_BOUNDS_VERIFIED, A733_BOUNDS_CLOSED };
struct a733_storage_bounds {
    enum a733_bounds_state state;
    const uint8_t *blob;
    size_t bytes;
    uint32_t writes, backups;
    uint8_t digest[32];            /* SHA-256 of the blob at parse time; allows() refuses a blob that no longer matches */
};
/* Check the blob (magic, counts, exact length, sorted disjoint nonempty ranges), keep a pointer to it and its SHA-256.
 * This OS maps all kernel data read-write at EL0 (docs/memory-protection.md), so no mapping can make the blob read-only:
 * allows() re-hashes it on every use and refuses a blob that has changed since parse. The image copies the runner's input
 * into private memory first, so the application's own input region is not the one parsed. Returns 1. */
int a733_storage_bounds_parse(struct a733_storage_bounds *, const uint8_t *blob, size_t bytes);
/* Read every backup sector in order with `read` (one sector per call, 512 bytes), hash the records (u64 little-endian
 * sector, 512 bytes) and compare. Returns 1 and opens the write ranges only when the hash matches. */
int a733_storage_bounds_verify(struct a733_storage_bounds *, int (*read)(void *, uint64_t sector, uint8_t *out), void *context);
/* 1 when verified and [first, first + blocks) lies inside one write range. */
int a733_storage_bounds_allows(const struct a733_storage_bounds *, uint64_t first, uint64_t blocks);
#include "sunxi_mmc.h"
#define A733_BOUNDED_WRITE_TOKEN ((uintptr_t)0x42574452u)
/* The authority for sunxi_mmc_write_dma and a733_mmc_platform_write_dma: admits a request carrying the bounded-writer token whose
 * whole range lies in one write range of a verified object. */
void a733_storage_bounds_authority(const struct a733_storage_bounds *, struct sunxi_mmc_write_authority *out);

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
/* Whether the blob still matches the hash taken at parse. */
int a733_storage_bounds_intact(const struct a733_storage_bounds *);
static inline int a733_storage_bounds_threshold_ok(const struct a733_storage_bounds *b)
{ return b && b->state == A733_BOUNDS_VERIFIED && a733_storage_bounds_intact(b); }
/* Whether a bounded writer may bind to this card: an SD card (kind 1), addressed by sector (SDHC and SDXC), with a
 * verified object. The register rule above takes the argument as a sector number. */
static inline int a733_storage_bounds_card_ok(const struct a733_storage_bounds *b, unsigned kind_is_sd, unsigned sector_addressed)
{ return kind_is_sd && sector_addressed && a733_storage_bounds_threshold_ok(b); }
#endif
