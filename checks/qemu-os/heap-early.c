/* Test of checks/qemu-os-boot.py: a call of the C library's sbrk before the application is admitted, as a static
 * constructor that calls malloc would make. The heap of the ledger can then no longer replace the linked heap, so
 * the admission must refuse the application instead of running it with a heap that is not the one it declared. */
#include <stddef.h>

extern void *_sbrk(ptrdiff_t change);
extern int __real_os_large_prepare(void);
int __wrap_os_large_prepare(void)
{
    (void)_sbrk(16);
    return __real_os_large_prepare();
}
