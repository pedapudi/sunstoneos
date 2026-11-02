#ifndef SUNSTONE_FAT32_RO_H
#define SUNSTONE_FAT32_RO_H
/* Read-only FAT32 reader for the large application region loader
 * (docs/designs/large-app-regions.md). It parses the boot sector and FSInfo,
 * walks the FAT and directories with long names, checks a file's cluster chain
 * against its size, and yields the file as contiguous sector extents that the
 * loader reads with pipelined requests. The format follows Microsoft's FAT
 * specification (FAT32 File System Specification, 2000-12-06). Every structure
 * that disagrees with itself is refused; nothing is repaired and nothing is
 * written. Sectors are 512 bytes. */
#include <stddef.h>
#include <stdint.h>

enum fat32_status {
    FAT32_OK = 0,
    FAT32_IO = -1,            /* the device refused or failed a read */
    FAT32_BAD_BOOT = -2,      /* boot sector or BPB fields are inconsistent */
    FAT32_UNSUPPORTED = -3,   /* valid but outside this reader: not FAT32, not 512-byte sectors */
    FAT32_BAD_CHAIN = -4,     /* free, reserved, bad or out-of-range cluster inside a chain */
    FAT32_BAD_DIR = -5,       /* malformed directory entry or directory chain */
    FAT32_NOT_FOUND = -6,
    FAT32_TRUNCATED = -7,     /* the chain ends before the file size is covered */
    FAT32_CHAIN_LONG = -8,    /* the chain continues past the file size, or loops */
    FAT32_CROSS_LINKED = -9,  /* a file cluster lies in its directory's chain */
    FAT32_INVALID = -10       /* argument error */
};

struct fat32_device {
    void *context;
    /* Read count sectors starting at an absolute sector into buffer: 1 on success. */
    int (*read)(void *, uint64_t sector, uint32_t count, uint8_t *buffer);
    uint64_t sectors;
};

#define FAT32_NAME_MAX 255u
#define FAT32_DIR_CLUSTERS_CHECKED 64u

struct fat32 {
    struct fat32_device device;
    uint64_t base, sectors, data_start;
    uint32_t sectors_per_cluster, reserved, fats, fat_sectors, root_cluster, clusters;
    uint32_t fsinfo_free, fsinfo_next;      /* 0xffffffff: not recorded */
    uint32_t volume_id;
    uint64_t fat_cached;                    /* absolute FAT sector in fat_cache, or UINT64_MAX */
    uint64_t dir_cached;
    uint8_t fat_cache[512], dir_cache[512];
};

struct fat32_entry {
    char name[FAT32_NAME_MAX + 1];
    uint8_t attributes, long_name;          /* long_name: 1 when the long entries verified */
    uint32_t first_cluster, size;
    uint32_t directory_cluster;             /* first cluster of the directory that holds it */
};
#define FAT32_ATTR_DIRECTORY 0x10u

struct fat32_dir {
    uint32_t first, cluster, index, walked;
};

/* base and sectors give the partition inside the device. */
int fat32_mount(struct fat32 *, const struct fat32_device *, uint64_t base, uint64_t sectors);
/* The cluster after a cluster: *next is 0 at the end of the chain. */
int fat32_next(struct fat32 *, uint32_t cluster, uint32_t *next);
/* Count the free clusters by reading the whole FAT through scratch (a multiple of 512 bytes). */
int fat32_count_free(struct fat32 *, uint8_t *scratch, uint32_t scratch_sectors, uint32_t *free_clusters);
uint64_t fat32_cluster_sector(const struct fat32 *, uint32_t cluster);

void fat32_dir_open(struct fat32 *, struct fat32_dir *, uint32_t first_cluster);
/* The next entry: 1 returned, 0 at the end, negative on error. Skips deleted entries,
 * the volume label and the dot entries. */
int fat32_dir_next(struct fat32 *, struct fat32_dir *, struct fat32_entry *);
/* Resolve a path of names separated by '/' from the root; names compare ASCII case-insensitively. */
int fat32_lookup(struct fat32 *, const char *path, struct fat32_entry *);

/* A file whose chain was walked and found to cover exactly its size. */
struct fat32_file {
    uint32_t first_cluster, size, clusters;
    uint32_t directory_cluster;
};
int fat32_open(struct fat32 *, const struct fat32_entry *, struct fat32_file *);

/* Extents of a file in order, each at most max_sectors sectors of contiguous card sectors. */
struct fat32_cursor {
    uint32_t cluster;                       /* next cluster to place, 0 at the end */
    uint64_t remaining;                     /* file bytes not yet covered by an extent */
    uint64_t run_sector;                    /* next sector of the run in progress */
    uint32_t run_sectors;                   /* sectors left in the run in progress */
};
void fat32_cursor_init(struct fat32_cursor *, const struct fat32_file *);
/* 1: an extent (sector, sectors, bytes of it that belong to the file); 0: done; negative: error. */
int fat32_cursor_next(struct fat32 *, struct fat32_cursor *, uint32_t max_sectors,
                      uint64_t *sector, uint32_t *sectors, uint64_t *bytes);

/* Pipelined load. The device reads one extent at a time into one of two buffers while the sink
 * consumes the other. A request in flight is the only device activity: chain lookups run between
 * a completion and the next submission. */
struct fat32_async {
    void *context;
    int (*submit)(void *, uint64_t sector, uint32_t count, uint8_t *buffer);   /* 1: started */
    int (*poll)(void *);                                                       /* 1 done, 0 busy, -1 failed */
};
struct fat32_sink {
    void *context;
    /* Consume bytes at offset; return 1 to continue. */
    int (*consume)(void *, const uint8_t *data, uint64_t bytes, uint64_t offset);
};
int fat32_stream(struct fat32 *, const struct fat32_file *, const struct fat32_async *,
                 uint8_t *buffer0, uint8_t *buffer1, uint32_t buffer_sectors, const struct fat32_sink *);
#endif
