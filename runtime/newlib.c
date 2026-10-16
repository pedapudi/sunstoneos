#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "runtime.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

extern char __heap_start[], __heap_end[];
static uintptr_t heap_break;
static size_t heap_peak;

size_t runtime_heap_peak(void) { return heap_peak; }

// The allocator serializes calls to _sbrk through these recursive locks.
// This port currently schedules tasks on one CPU; it does not permit ISR allocation.
void __malloc_lock(struct _reent *context)
{
    (void)context;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        vTaskSuspendAll();
}

void __malloc_unlock(struct _reent *context)
{
    (void)context;
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
        (void)xTaskResumeAll();
}

void *_sbrk(ptrdiff_t change)
{
    if (!heap_break) heap_break = (uintptr_t)__heap_start;
    uintptr_t previous = heap_break;
    uintptr_t magnitude = change < 0 ? (uintptr_t)(-(change + 1)) + 1 : (uintptr_t)change;
    if ((change >= 0 && magnitude > (uintptr_t)__heap_end - heap_break) ||
        (change < 0 && magnitude > heap_break - (uintptr_t)__heap_start)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_break = change < 0 ? heap_break - magnitude : heap_break + magnitude;
    size_t used = heap_break - (uintptr_t)__heap_start;
    if (used > heap_peak) heap_peak = used;
    return (void *)previous;
}

ssize_t _write(int descriptor, const void *buffer, size_t count)
{
    if (descriptor != 1 && descriptor != 2) {
        errno = EBADF;
        return -1;
    }
    const char *text = buffer;
    for (size_t i = 0; i < count; ++i) {
        char character[2] = {text[i], 0};
        platform_puts(character);
    }
    return (ssize_t)count;
}

int _fstat(int descriptor, struct stat *info)
{
    if (descriptor != 1 && descriptor != 2) { errno = EBADF; return -1; }
    memset(info, 0, sizeof(*info));
    info->st_mode = S_IFCHR;
    return 0;
}
int _isatty(int descriptor)
{
    if (descriptor == 1 || descriptor == 2) return 1;
    errno = EBADF;
    return 0;
}
// No platform-specific finalizers; normal C++ destruction uses __cxa_atexit.
void _fini(void) {}

int _close(int descriptor) { (void)descriptor; errno = EBADF; return -1; }
ssize_t _read(int descriptor, void *buffer, size_t count)
{
    (void)descriptor; (void)buffer; (void)count;
    errno = ENOSYS;
    return -1;
}
off_t _lseek(int descriptor, off_t offset, int whence)
{
    (void)descriptor; (void)offset; (void)whence;
    errno = ESPIPE;
    return -1;
}
int _getpid(void) { return 1; }
int _kill(int process, int signal)
{
    (void)process; (void)signal; errno = ENOSYS; return -1;
}
_Noreturn void _exit(int status) { platform_exit((unsigned)status); }

void runtime_init(void)
{
    extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
    for (void (**entry)(void) = __init_array_start; entry != __init_array_end; ++entry)
        (*entry)();
}
