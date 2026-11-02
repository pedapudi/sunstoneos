#include "sunstone_fs.h"
#include <errno.h>
#include <string.h>

int ss_errno(int s)
{
    switch (s) {
    case FWR_OK: return 0;
    case FWR_NOT_FOUND: return ENOENT;
    case FWR_EXISTS: return EEXIST;
    case FWR_FULL: return ENOSPC;
    case FWR_INVALID: return EINVAL;
    case FWR_BUSY: return EBUSY;
    case FWR_REFUSED: return EACCES;
    case FWR_NOT_EMPTY: return ENOTEMPTY;
    case FWR_UNSUPPORTED: return ENOTSUP;
    default: return EIO;
    }
}
#define FAIL(s) (-ss_errno(s))

int ss_fs_init(struct ss_fs *fs, struct fwr_volume *v, uint8_t *pool, size_t buffer_bytes)
{
    if (!fs || !v || !pool || buffer_bytes < FWR_SECTOR || buffer_bytes % FWR_SECTOR) return -EINVAL;
    memset(fs, 0, sizeof *fs);
    fs->volume = v; fs->pool = pool; fs->buffer_bytes = buffer_bytes;
    for (unsigned i = 0; i < SS_FS_MAX_FILES; ++i) fs->files[i].buffer = pool + (size_t)i * buffer_bytes;
    return 0;
}

#define FD_BASE 3
static struct ss_fs_file *get(struct ss_fs *fs, int fd)
{
    if (fd < FD_BASE || fd >= FD_BASE + (int)SS_FS_MAX_FILES) return NULL;
    struct ss_fs_file *f = &fs->files[fd - FD_BASE];
    return f->used ? f : NULL;
}

int ss_open(struct ss_fs *fs, const char *path, int flags)
{
    if (!fs || !path || strlen(path) >= SS_FS_PATH_MAX) return -EINVAL;
    unsigned i = 0;
    while (i < SS_FS_MAX_FILES && fs->files[i].used) ++i;
    if (i == SS_FS_MAX_FILES) return -EMFILE;
    struct ss_fs_file *f = &fs->files[i];
    int access = flags & 3;
    uint64_t size = 0;
    int is_dir = 0, exists = fwr_exists(fs->volume, path, &size, &is_dir) == FWR_OK;
    if (is_dir) return -EISDIR;
    memset(&f->file, 0, sizeof f->file);
    f->position = 0; f->readable = access != SS_O_WRONLY; f->writable = access != SS_O_RDONLY;
    int r;
    if (!f->writable) {
        if (!exists) return -ENOENT;
    } else {
        unsigned mode;
        if (flags & SS_O_TRUNC) { if (!(flags & SS_O_CREAT) && !exists) return -ENOENT; mode = FWR_O_REPLACE; }
        else if (exists) {
            if ((flags & SS_O_CREAT) && (flags & SS_O_EXCL)) return -EEXIST;
            /* Writing at the start of existing data would overwrite it in place. */
            if (!(flags & SS_O_APPEND) && size) return -ENOTSUP;
            mode = FWR_O_APPEND;
        } else {
            if (!(flags & SS_O_CREAT)) return -ENOENT;
            mode = FWR_O_CREATE | ((flags & SS_O_APPEND) ? FWR_O_APPEND : 0);
        }
        r = fwr_open(fs->volume, &f->file, path, mode, fs->prealloc_default, f->buffer, fs->buffer_bytes);
        if (r) return FAIL(r);
        f->position = f->file.size;
    }
    strcpy(f->path, path);
    f->used = 1;
    return (int)i + FD_BASE;
}

ssize_t ss_write_nowait(struct ss_fs *fs, int fd, const void *data, size_t n)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f || !f->writable) return -EBADF;
    if (f->file.failed) return -EIO;
    size_t k = fwr_write(&f->file, data, n);
    f->position = f->file.size;
    return (ssize_t)k;
}

ssize_t ss_write(struct ss_fs *fs, int fd, const void *data, size_t n)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f || !f->writable) return -EBADF;
    const uint8_t *p = data;
    size_t done = 0;
    while (done < n) {
        size_t k = fwr_write(&f->file, p + done, n - done);
        done += k;
        if (f->file.failed) return -EIO;
        if (done < n || k == 0) {
            int64_t r = fwr_service(&f->file, 0);
            if (r < 0) return FAIL((int)r);
        }
    }
    f->position = f->file.size;
    return (ssize_t)done;
}

int64_t ss_fs_service(struct ss_fs *fs)
{
    int64_t total = 0;
    for (unsigned i = 0; i < SS_FS_MAX_FILES; ++i) {
        if (!fs->files[i].used || !fs->files[i].writable) continue;
        int64_t r = fwr_service(&fs->files[i].file, 0);
        if (r < 0) return r;
        total += r;
    }
    return total;
}

ssize_t ss_read(struct ss_fs *fs, int fd, void *out, size_t n)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f || !f->readable) return -EBADF;
    if (f->writable && f->file.size != f->file.synced_size) {
        int r = fwr_sync(&f->file);
        if (r) return FAIL(r);
    }
    size_t got = 0;
    int r = fwr_read(fs->volume, f->path, f->position, out, n, &got);
    if (r) return FAIL(r);
    f->position += got;
    return (ssize_t)got;
}

int ss_fsync(struct ss_fs *fs, int fd)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f) return -EBADF;
    if (!f->writable) return 0;
    int r = fwr_sync(&f->file);
    return r ? FAIL(r) : 0;
}

int ss_close(struct ss_fs *fs, int fd)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f) return -EBADF;
    int r = f->writable ? fwr_close(&f->file) : 0;
    f->used = 0;
    return r ? FAIL(r) : 0;
}

int64_t ss_lseek(struct ss_fs *fs, int fd, int64_t offset, int whence)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f) return -EBADF;
    int64_t size = f->writable ? (int64_t)f->file.size : 0;
    if (!f->writable) {
        uint64_t s = 0;
        if (fwr_exists(fs->volume, f->path, &s, NULL)) return -EIO;
        size = (int64_t)s;
    }
    int64_t target = whence == SS_SEEK_SET ? offset : whence == SS_SEEK_CUR ? (int64_t)f->position + offset
                   : whence == SS_SEEK_END ? size + offset : -1;
    if (target < 0) return -EINVAL;
    if (f->writable && (uint64_t)target != f->file.size) return -ESPIPE;
    f->position = (uint64_t)target;
    return target;
}

int ss_ftruncate(struct ss_fs *fs, int fd, uint64_t size)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f || !f->writable) return -EBADF;
    int r = fwr_truncate(&f->file, size);
    f->position = f->file.size;
    return r ? FAIL(r) : 0;
}

int ss_fallocate_keep(struct ss_fs *fs, int fd, uint64_t length)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f || !f->writable) return -EBADF;
    int r = fwr_preallocate(&f->file, length);
    return r ? FAIL(r) : 0;
}

int ss_posix_fallocate(struct ss_fs *fs, int fd, uint64_t offset, uint64_t length)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f || !f->writable) return -EBADF;
    if (!length) return -EINVAL;
    uint64_t end = offset + length;
    int r = fwr_preallocate(&f->file, end);
    if (r) return FAIL(r);
    if (end > f->file.size) { r = fwr_truncate(&f->file, end); if (r) return FAIL(r); }
    return 0;
}

int ss_rename(struct ss_fs *fs, const char *from, const char *to)
{
    int r = fwr_rename(fs->volume, from, to);
    return r == FWR_UNSUPPORTED ? -EXDEV : r ? FAIL(r) : 0;
}
int ss_unlink(struct ss_fs *fs, const char *path) { int r = fwr_unlink(fs->volume, path); return r ? FAIL(r) : 0; }
int ss_mkdir(struct ss_fs *fs, const char *path) { int r = fwr_mkdir(fs->volume, path); return r ? FAIL(r) : 0; }

int ss_stat(struct ss_fs *fs, const char *path, struct ss_stat *st)
{
    uint64_t size; int dir;
    int r = fwr_exists(fs->volume, path, &size, &dir);
    if (r) return FAIL(r);
    st->size = size; st->is_directory = dir;
    return 0;
}
int ss_fstat(struct ss_fs *fs, int fd, struct ss_stat *st)
{
    struct ss_fs_file *f = get(fs, fd);
    if (!f) return -EBADF;
    if (f->writable) { st->size = f->file.size; st->is_directory = 0; return 0; }
    return ss_stat(fs, f->path, st);
}
const struct fwr_wear *ss_wear(const struct ss_fs *fs) { return fwr_wear(fs->volume); }
