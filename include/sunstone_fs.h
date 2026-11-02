#ifndef SUNSTONE_FS_H
#define SUNSTONE_FS_H
/* POSIX-like file calls for applications (docs/designs/storage-write.md). They sit on the FAT32
 * write service (fat32_rw.h): writes go to a RAM buffer, the buffer flushes in allocation-unit
 * writes, and fsync is the durability point. Every call returns a non-negative value or a negated
 * errno value. The newlib system calls in sunstone_fs_syscalls.c wrap these, so fopen, fwrite,
 * fflush and fclose work on the same files.
 *
 * Files are append-only: a file opened for writing continues at its end. Existing data is not
 * overwritten in place (O_TRUNC replaces the file; see rename and the replace rules below). */
#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#include "fat32_rw.h"

#define SS_FS_MAX_FILES 8u
#define SS_FS_PATH_MAX 256u

/* open flags (the values newlib uses on AArch64 bare metal) */
#define SS_O_RDONLY 0x0000
#define SS_O_WRONLY 0x0001
#define SS_O_RDWR   0x0002
#define SS_O_APPEND 0x0008
#define SS_O_CREAT  0x0200
#define SS_O_TRUNC  0x0400
#define SS_O_EXCL   0x0800
#define SS_SEEK_SET 0
#define SS_SEEK_CUR 1
#define SS_SEEK_END 2

struct ss_fs_file {
    struct fwr_file file;
    char path[SS_FS_PATH_MAX];
    uint64_t position;
    uint8_t *buffer;
    uint8_t used, writable, readable;
};

struct ss_fs {
    struct fwr_volume *volume;
    struct ss_fs_file files[SS_FS_MAX_FILES];
    uint8_t *pool;               /* SS_FS_MAX_FILES buffers of buffer_bytes each */
    size_t buffer_bytes;
    uint64_t prealloc_default;   /* clusters reserved when a file is created, in bytes; 0: none */
};

struct ss_stat { uint64_t size; int is_directory; };

/* pool holds SS_FS_MAX_FILES buffers of buffer_bytes (a multiple of 512, two allocation units advised). */
int ss_fs_init(struct ss_fs *, struct fwr_volume *, uint8_t *pool, size_t buffer_bytes);

int ss_open(struct ss_fs *, const char *path, int flags);
/* Blocking write for a task that may wait for the card: accepts all n bytes, flushing as the buffer fills. */
ssize_t ss_write(struct ss_fs *, int fd, const void *data, size_t n);
/* Real-time write: copies into the buffer and returns the number accepted (0 when full, which a
 * caller treats as would-block). It never touches the card. */
ssize_t ss_write_nowait(struct ss_fs *, int fd, const void *data, size_t n);
/* Writer task: flush every file whose buffer holds at least one allocation unit. Returns bytes flushed. */
int64_t ss_fs_service(struct ss_fs *);
/* Reads durable bytes at the descriptor position. On a writable descriptor, unsynced data is synced first. */
ssize_t ss_read(struct ss_fs *, int fd, void *out, size_t n);
int ss_fsync(struct ss_fs *, int fd);
int ss_close(struct ss_fs *, int fd);
/* Only position queries and SEEK_END on read descriptors: a writable descriptor stays at its end. */
int64_t ss_lseek(struct ss_fs *, int fd, int64_t offset, int whence);
int ss_ftruncate(struct ss_fs *, int fd, uint64_t size);
/* posix_fallocate: space for [offset, offset + length) is reserved and the size extends to the end
 * with zero bytes. ss_fallocate_keep reserves clusters and leaves the size alone (Linux
 * FALLOC_FL_KEEP_SIZE); that is the wear-friendly form. */
int ss_posix_fallocate(struct ss_fs *, int fd, uint64_t offset, uint64_t length);
int ss_fallocate_keep(struct ss_fs *, int fd, uint64_t length);
/* Atomic replace within one directory (-EXDEV across directories). */
int ss_rename(struct ss_fs *, const char *from, const char *to);
int ss_unlink(struct ss_fs *, const char *path);
int ss_mkdir(struct ss_fs *, const char *path);
int ss_stat(struct ss_fs *, const char *path, struct ss_stat *);
int ss_fstat(struct ss_fs *, int fd, struct ss_stat *);
const struct fwr_wear *ss_wear(const struct ss_fs *);
int ss_errno(int fwr_status);
#endif
