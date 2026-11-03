/* Test application of checks/qemu-os-boot.py for the start-up order of a heap from the application memory ledger
 * (docs/designs/application-memory-layout.md, step 2b). A static constructor takes 4096 bytes from sbrk before the
 * application's main task runs. The constructors run after the admission has attached the ledger's region as the heap,
 * so the block lies at the start of that region, is zero, and the main task's own first block follows it. An image
 * that ran the constructors first would take the block from the linked heap and refuse the region.
 * Result {tag, constructor block, 1 if it was not zero, first block of the main task, heap peak}. */
#include "heap_test-app.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/reent.h>
#include <sys/types.h>

extern void *_sbrk(ptrdiff_t change);
extern size_t runtime_heap_peak(void);
static struct _reent test_reent;
struct _reent *_impure_ptr = &test_reent;
/* The C library is not linked into the virtual board: the references that SUNSTONE_NEWLIB adds are met here. */
void _reclaim_reent(struct _reent *context) { (void)context; }
FILE __sf[3];   /* the per-task reentrancy initialization of the kernel under SUNSTONE_NEWLIB names it */

static void *constructor_block;
static unsigned constructor_dirty;
__attribute__((constructor)) static void take_early(void)
{
    constructor_block = _sbrk(4096);
    if (constructor_block == (void *)-1) return;
    for (unsigned i = 0; i < 4096; ++i) constructor_dirty |= ((volatile uint8_t *)constructor_block)[i];
    memset(constructor_block, 0xa5, 4096);
}

void heap_test_main(const struct ss_grant *grant)
{
    (void)grant;
    void *own = _sbrk(4096);
    const uint64_t result[5] = {UINT64_C(0x4354524f48454150), (uintptr_t)constructor_block, constructor_dirty,
                                (uintptr_t)own, runtime_heap_peak()};
    ss_finish(result, sizeof result);
}
