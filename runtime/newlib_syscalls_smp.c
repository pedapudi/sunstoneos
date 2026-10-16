#include <reent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

extern ssize_t _write(int descriptor, const void *buffer, size_t count);
extern int _fstat(int descriptor, struct stat *info);

// Newlib is built with REENTRANT_SYSCALLS_PROVIDED. Errors go directly to
// the supplied task state, without the library's global-errno wrappers.
ssize_t _write_r(struct _reent *context, int fd, const void *buffer, size_t count) {
    if (fd != 1 && fd != 2) { context->_errno = EBADF; return -1; }
    return _write(fd, buffer, count);
}
int _fstat_r(struct _reent *context, int fd, struct stat *info) {
    if (fd != 1 && fd != 2) { context->_errno = EBADF; return -1; }
    return _fstat(fd, info);
}
int _isatty_r(struct _reent *context, int fd) {
    if (fd == 1 || fd == 2) return 1;
    context->_errno = EBADF; return 0;
}
int _close_r(struct _reent *context, int fd) {
    (void)fd; context->_errno = EBADF; return -1;
}
ssize_t _read_r(struct _reent *context, int fd, void *buffer, size_t count) {
    (void)fd; (void)buffer; (void)count; context->_errno = ENOSYS; return -1;
}
_off_t _lseek_r(struct _reent *context, int fd, _off_t offset, int whence) {
    (void)fd; (void)offset; (void)whence; context->_errno = ESPIPE; return -1;
}
int _getpid_r(struct _reent *context) { (void)context; return 1; }
int _kill_r(struct _reent *context, int pid, int signal) {
    (void)pid; (void)signal; context->_errno = ENOSYS; return -1;
}
