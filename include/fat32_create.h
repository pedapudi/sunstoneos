#ifndef SUNSTONE_FAT32_CREATE_H
#define SUNSTONE_FAT32_CREATE_H
/* Create one contiguous file on a FAT32 volume with a bounded set of sector writes
 * (docs/designs/region-test-file-write.md). The module plans first, from reads only: the first free
 * cluster run that holds the file, a free run of directory slots in the root directory's first
 * cluster, the FAT sectors the chain touches and the FSInfo sector. It then writes in a fixed order:
 * data clusters, the chain in every FAT copy, the directory entries, FSInfo. Every sector goes
 * through one guard that refuses a sector outside the sets above, whatever the plan fields say: the
 * bounds are recomputed from the volume geometry and the plan's first cluster and cluster count. */
#include "fat32_ro.h"

enum fat32_create_status {
    FAT32_CREATE_OK = 0,
    FAT32_CREATE_IO = -1,
    FAT32_CREATE_NO_SPACE = -2,       /* no contiguous free run of the size */
    FAT32_CREATE_EXISTS = -3,         /* the name, long or short, is already in the directory */
    FAT32_CREATE_NO_SLOT = -4,        /* no free run of directory entries in the root cluster */
    FAT32_CREATE_OUT_OF_BOUNDS = -5,  /* a write outside the bounded set, refused before it started */
    FAT32_CREATE_INVALID = -6,
    FAT32_CREATE_VOLUME = -7,         /* FAT copies differ in the touched range, or FSInfo is unusable */
    FAT32_CREATE_NOT_FREE = -8        /* a chain entry that the plan expected to be free is not */
};

#define FAT32_CREATE_MAX_ENTRIES 22u    /* long name entries (up to 20) and the short entry */

struct fat32_create_plan {
    uint32_t first_cluster, clusters;
    uint64_t size;
    uint32_t free_before, next_before;
    char short_name[12];                  /* 8.3 without the dot, space padded */
    uint32_t entry_index, entry_count;    /* first slot in the root cluster, slots used */
    uint8_t entries[FAT32_CREATE_MAX_ENTRIES][32];
    uint16_t date, time;                  /* creation stamps written into the short entry */
};

struct fat32_create_io {
    void *context;
    int (*write)(void *, uint64_t sector, uint32_t count, const uint8_t *data);   /* 1 on success */
    /* The file's bytes: fill out with bytes of the file from offset; 1 on success. */
    int (*fill)(void *, uint64_t offset, uint8_t *out, uint32_t bytes);
};

/* Plan from reads only. scratch holds scratch_sectors sectors for scanning the FAT. */
int fat32_create_plan(struct fat32 *, const char *name, uint64_t size, uint16_t date, uint16_t time,
                      uint8_t *scratch, uint32_t scratch_sectors, struct fat32_create_plan *);
/* The guard: 1 when sector is in the set the plan may write on this volume. */
int fat32_create_allowed(const struct fat32 *, const struct fat32_create_plan *, uint64_t sector);
/* The bounded sets, for the review list: data extent, FAT sectors per copy, FSInfo, root cluster. */
struct fat32_create_bounds {
    uint64_t data_first, data_sectors;
    uint64_t fat_first[2], fat_sectors;
    uint32_t fats;
    uint64_t fsinfo, root_first, root_sectors;
};
int fat32_create_bounds(const struct fat32 *, const struct fat32_create_plan *, struct fat32_create_bounds *);
/* Which part of the creation one call performs. The data clusters are free space until the chain is
 * written, so the data may be written in several calls (several lifetimes); the chain, the directory entries
 * and FSInfo follow in that order, each call for its phase. A null run is the whole creation in one call. */
#define FAT32_CREATE_DATA 1u
#define FAT32_CREATE_FAT 2u
#define FAT32_CREATE_DIR 4u
#define FAT32_CREATE_INFO 8u
struct fat32_create_run {
    unsigned phases;
    uint64_t data_from, data_sectors;     /* the slice of the data extent, in sectors from its start */
};
/* Write in order. buffer holds buffer_sectors sectors. The write callback sees only allowed sectors.
 * Every call re-checks that the chain entries and the directory slots are still free, except that a call
 * for the FAT, DIR or INFO phase alone does not repeat the check of the phases already done. */
int fat32_create_execute(struct fat32 *, const struct fat32_create_plan *, const struct fat32_create_io *,
                         uint8_t *buffer, uint32_t buffer_sectors, const struct fat32_create_run *run);

/* Read-only consistency report of a volume: both FATs equal, FSInfo against the counted free clusters. */
struct fat32_consistency {
    uint32_t fats_equal, fsinfo_valid, fsinfo_free, counted_free, fsinfo_next;
    uint64_t differing_fat_sectors, first_difference;
};
int fat32_check_volume(struct fat32 *, uint8_t *scratch_a, uint8_t *scratch_b, uint32_t scratch_sectors,
                       struct fat32_consistency *);

/* The test file's bytes: counter-mode SHA-256, as tools/region_test_file.py. */
int region_test_fill(void *unused, uint64_t offset, uint8_t *out, uint32_t bytes);
#endif
