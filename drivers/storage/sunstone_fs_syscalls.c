/* newlib system calls over the POSIX-like file calls (sunstone_fs.h), so fopen, fwrite, fflush,
 * fclose, rename and remove work on the card. Descriptors 0 to 2 go to the console hooks.
 * Link this file only in an application that uses the card; the default instance is bound once
 * with ss_syscalls_bind. SS_SYS lets a host check compile the same code under other names. */
#include "sunstone_fs.h"
#include <errno.h>
#include <sys/stat.h>

#ifndef SS_SYS
#define SS_SYS(name) name
#endif

static struct ss_fs *bound;
void ss_syscalls_bind(struct ss_fs *fs) { bound = fs; }

__attribute__((weak)) int ss_console_write(int fd, const char *data, int n) { (void)fd; (void)data; return n; }

static int result(long r) { if (r < 0) { errno = (int)-r; return -1; } return (int)r; }

int SS_SYS(_open)(const char *path, int flags, int mode)
{ (void)mode; return bound ? result(ss_open(bound, path, flags)) : (errno = ENODEV, -1); }
int SS_SYS(_close)(int fd) { return fd < 3 ? 0 : bound ? result(ss_close(bound, fd)) : (errno = EBADF, -1); }
int SS_SYS(_write)(int fd, const char *data, int n)
{ if (fd < 3) return ss_console_write(fd, data, n); return bound ? result(ss_write(bound, fd, data, (size_t)n)) : (errno = EBADF, -1); }
int SS_SYS(_read)(int fd, char *out, int n)
{ return fd < 3 || !bound ? (errno = EBADF, -1) : result(ss_read(bound, fd, out, (size_t)n)); }
long SS_SYS(_lseek)(int fd, long offset, int whence)
{ return fd < 3 || !bound ? (errno = ESPIPE, -1) : result((long)ss_lseek(bound, fd, offset, whence)); }
int SS_SYS(_isatty)(int fd) { return fd < 3; }
int SS_SYS(_fstat)(int fd, struct stat *st)
{
    st->st_mode = S_IFCHR;
    if (fd < 3) return 0;
    struct ss_stat s;
    if (!bound || result(ss_fstat(bound, fd, &s)) < 0) return -1;
    st->st_mode = S_IFREG; st->st_size = (off_t)s.size;
    return 0;
}
int SS_SYS(_stat)(const char *path, struct stat *st)
{
    struct ss_stat s;
    if (!bound || result(ss_stat(bound, path, &s)) < 0) return -1;
    st->st_mode = s.is_directory ? S_IFDIR : S_IFREG; st->st_size = (off_t)s.size;
    return 0;
}
int SS_SYS(_unlink)(const char *path) { return bound ? result(ss_unlink(bound, path)) : (errno = ENODEV, -1); }
int SS_SYS(_rename)(const char *from, const char *to) { return bound ? result(ss_rename(bound, from, to)) : (errno = ENODEV, -1); }
int SS_SYS(fsync)(int fd) { return bound ? result(ss_fsync(bound, fd)) : (errno = EBADF, -1); }
int SS_SYS(ftruncate)(int fd, off_t size) { return bound ? result(ss_ftruncate(bound, fd, (uint64_t)size)) : (errno = EBADF, -1); }
/* posix_fallocate returns the error number instead of setting errno. */
int SS_SYS(posix_fallocate)(int fd, off_t offset, off_t length)
{ return bound ? -ss_posix_fallocate(bound, fd, (uint64_t)offset, (uint64_t)length) : EBADF; }
