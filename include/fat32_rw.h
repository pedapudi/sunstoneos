#ifndef SUNSTONE_FAT32_RW_H
#define SUNSTONE_FAT32_RW_H
/* General FAT32 write service for a microSD card (docs/designs/storage-write.md).
 *
 * Applications write through a RAM buffer. The service flushes data in writes that end on
 * allocation-unit boundaries, reserves clusters in memory (the FAT does not change until a
 * sync), and updates the FAT, the directory entry and the volume flags only at sync points,
 * in an order that leaves a recoverable volume after a power cut. Append-only files: there is
 * no seek into written data. Sectors are 512 bytes. The format follows the Microsoft FAT32
 * File System Specification (2000-12-06). One task owns a volume; calls do not nest. */
#include <stddef.h>
#include <stdint.h>

enum fwr_status {
    FWR_OK = 0,
    FWR_IO = -1,
    FWR_BAD_VOLUME = -2,       /* boot sector, FAT or FSInfo disagree with themselves */
    FWR_UNSUPPORTED = -3,
    FWR_NOT_FOUND = -4,
    FWR_EXISTS = -5,
    FWR_FULL = -6,             /* no free cluster or directory space */
    FWR_INVALID = -7,          /* argument error or an unusable name */
    FWR_REFUSED = -8,          /* a sector outside the FAT, FSInfo, directory and data areas */
    FWR_BUSY = -9,             /* the file or volume has open files */
    FWR_NEEDS_RECOVERY = -10,  /* dirty volume and no recovery scratch supplied */
    FWR_CORRUPT = -11,         /* recovery found a structure it cannot repair */
    FWR_NOT_EMPTY = -12,
    FWR_FAILED = -13           /* an earlier error latched on the file */
};

#define FWR_SECTOR 512u
#define FWR_NAME_MAX 255u
#define FWR_MAX_RUNS 8u
#define FWR_FAT_WINDOW_SECTORS 128u
#define FWR_HIST 16u

struct fwr_device {
    void *context;
    /* Both return 1 on success. A write of count sectors persists in ascending sector order. */
    int (*read)(void *, uint64_t sector, uint32_t count, uint8_t *buffer);
    int (*write)(void *, uint64_t sector, uint32_t count, const uint8_t *buffer);
    /* Optional: called before each write piece so a pending read can run first. */
    void (*yield)(void *);
    uint64_t sectors;
    /* Allocation unit in sectors from the SD Status register (ACMD13), absolute to the card;
     * 0 when unknown, which selects FWR_DEFAULT_AU_SECTORS. */
    uint32_t au_sectors;
    /* Largest single write (one CMD25 with descriptor DMA). 0 selects 256 sectors. */
    uint32_t max_write_sectors;
};
#define FWR_DEFAULT_AU_SECTORS 128u

struct fwr_config {
    struct fwr_device device;
    uint64_t base, sectors;            /* the partition inside the device */
    uint8_t *cluster_buffer;           /* at least one cluster, 64-byte aligned */
    size_t cluster_buffer_bytes;
    uint8_t *fat_window;               /* 2 * FWR_FAT_WINDOW_SECTORS * 512 bytes: window and compare area */
    uint8_t *recovery_bitmap;          /* (clusters + 2 + 7) / 8 bytes; NULL disables recovery */
    size_t recovery_bitmap_bytes;
    uint32_t (*now)(void *);           /* FAT date << 16 | FAT time; NULL selects 1980-01-01 */
    void *now_context;
    uint64_t grow_bytes;               /* reservation step; 0 selects 16 MiB */
    /* Optional narrowing of the writable data area to the clusters at or after this absolute
     * sector (for the board's bounded plan); 0 = whole partition. */
    uint64_t write_floor;
};

struct fwr_wear {
    uint64_t bytes_written, data_bytes, metadata_bytes;
    uint64_t writes, flushes, syncs;
    uint64_t au_aligned_writes, unaligned_writes;
    uint64_t size_histogram[FWR_HIST];     /* bucket i: writes of 2^(i-1)+1 .. 2^i sectors; 0: one sector */
    uint64_t recoveries, recovered_clusters;
};

struct fwr_file;
struct fwr_run { uint32_t start, length; };

struct fwr_volume {
    struct fwr_config cfg;
    uint64_t data_start, fat1, fat2, fsinfo, total_end;
    uint32_t spc, csize, reserved, fat_sectors, root, clusters, au, max_write;
    uint32_t free_clusters, next_hint;     /* 0xffffffff: unknown */
    uint8_t fsinfo_valid, dirty_set, fats;
    uint32_t window_first;                 /* FAT sector index of the window start; ~0u: none */
    uint32_t window_valid_sectors;
    uint8_t in_recovery;
    uint8_t window_dirty[FWR_FAT_WINDOW_SECTORS / 8];
    uint64_t dir_cached;
    uint8_t dir_sector[FWR_SECTOR];
    struct fwr_file *files;
    struct fwr_wear wear;
    uint32_t volume_flag_fat1;             /* last FAT[1] value written */
};

struct fwr_slot { uint64_t sector; uint8_t index; };
struct fwr_loc {
    struct fwr_slot sfn;
    uint8_t lfn_count;
    struct fwr_slot lfn[20];
    uint8_t entry[32];
    uint32_t directory;                    /* first cluster of the containing directory */
};

enum { FWR_O_CREATE = 1, FWR_O_APPEND = 2, FWR_O_REPLACE = 4 };

struct fwr_file {
    struct fwr_volume *volume;
    struct fwr_file *next;
    uint8_t mode, failed, has_entry, closed;
    uint32_t directory;                    /* parent directory first cluster */
    char name[FWR_NAME_MAX + 1];
    struct fwr_loc loc;
    uint32_t first_cluster, tail, chained;       /* chained clusters on disk */
    uint64_t synced_size, size;                  /* size in the directory entry; bytes accepted */
    uint64_t written_to;                         /* logical bytes on the card; buffer[0] is at written_to & ~511 */
    uint8_t *buffer;
    size_t capacity;
    struct fwr_run runs[FWR_MAX_RUNS];           /* reserved, not yet chained, in file order */
    uint32_t run_count;
    uint8_t replace_pending;                     /* FWR_O_REPLACE not yet published */
};

/* Mount. A volume whose FAT[1] dirty flag is set is recovered first (needs recovery_bitmap). */
int fwr_mount(struct fwr_volume *, const struct fwr_config *);
/* Writes the FSInfo hint, then clears the dirty flag. FWR_BUSY while files are open. */
int fwr_unmount(struct fwr_volume *);
/* Writes FSInfo and, when no file has unsynced data, clears the dirty flag. */
int fwr_checkpoint(struct fwr_volume *);
/* Rename within one directory. An existing destination file is replaced: the destination
 * entry switches to the source chain in one sector write, then the source entry is removed. */
int fwr_rename(struct fwr_volume *, const char *from, const char *to);

int fwr_mkdir(struct fwr_volume *, const char *path);
int fwr_unlink(struct fwr_volume *, const char *path);
int fwr_exists(struct fwr_volume *, const char *path, uint64_t *size, int *is_directory);

/* Open for writing. FWR_O_CREATE fails when the path exists; FWR_O_APPEND continues an
 * existing file (FWR_O_CREATE|FWR_O_APPEND creates when absent); FWR_O_REPLACE writes a new
 * body that replaces the path (created when absent): the first sync makes data and chain
 * durable, then switches the directory entry in one sector write. prealloc_bytes reserves clusters, contiguous where
 * the volume has a run. buffer and capacity (a multiple of 512, at least two allocation
 * units advised) belong to the file until close. */
int fwr_open(struct fwr_volume *, struct fwr_file *, const char *path, unsigned mode,
             uint64_t prealloc_bytes, uint8_t *buffer, size_t capacity);
/* Buffers up to n bytes without touching the card and returns the number accepted (the
 * buffer may be full). */
size_t fwr_write(struct fwr_file *, const void *data, size_t n);
/* Writer-task call: flushes aligned data when at least one allocation unit is buffered, or
 * everything when force is set. Returns bytes flushed or a negative status. */
int64_t fwr_service(struct fwr_file *, int force);
/* Makes everything written so far durable (data, FAT, directory entry). */
int fwr_sync(struct fwr_file *);
/* Syncs, commits a replace, releases unused reservations. The file object is closed even on error. */
int fwr_close(struct fwr_file *);
/* Sets the final size at or below the written size (ftruncate semantic) after syncing. */
int fwr_truncate(struct fwr_file *, uint64_t size);
/* posix_fallocate semantic: reserve clusters so the file can reach `size` bytes. */
int fwr_preallocate(struct fwr_file *, uint64_t size);

/* Sequential read of a whole file's bytes (verification and small config reads). */
int fwr_read(struct fwr_volume *, const char *path, uint64_t offset, void *out, size_t n, size_t *got);

const struct fwr_wear *fwr_wear(const struct fwr_volume *);
/* 1 when a write of count sectors at an absolute sector lies wholly inside the FAT copies, the
 * FSInfo sector or the writable data area; every service write passes this test first. */
int fwr_write_allowed(const struct fwr_volume *, uint64_t sector, uint32_t count);
#endif
