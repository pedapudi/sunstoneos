/* Local-layout pool preparation against a modeled translation callback.
 * The runner links the pool sources with --defsym pool addresses and counts
 * the write translations the boot preparation makes: one per page of either
 * pool, or one per 2 MiB block that holds a pool byte when built with
 * A733_LOCAL_POOLS_BLOCK_MAPPED. */
#include "a733_memory_local.h"
#include "a733_memory_reservations.h"
#include "a733_memory_uncached.h"
#include "aarch64_device_memory.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned translations, fault_block = UINT32_MAX;
unsigned aarch64_device_cache_stride(void) { return 64; }
int aarch64_device_cache(enum aarch64_cache_op op, uintptr_t cpu, size_t bytes)
{
    (void)op; (void)cpu; (void)bytes;
    return 1;
}
int aarch64_device_translate(uintptr_t address, int write, struct aarch64_translation *r)
{
    (void)write;
    ++translations;
    if ((address >> 21) == fault_block) return 0;
    int nc = address >= 0x41c00000 && address < 0x41e00000;
    *r = (struct aarch64_translation){address, nc ? 0x44u : 0xffu, 3};
    return 1;
}

int main(int argc, char **argv)
{
    /* argv[1]: block index whose translation faults, or none. */
    if (argc > 1) fault_block = (unsigned)strtoul(argv[1], NULL, 0);
    /* The getters do not prepare: before the boot preparation they return
     * NULL and translate nothing. */
    assert(!a733_memory_reservations_init() && !a733_memory_uncached_init() && !translations);
    int wb_ready = a733_memory_reservations_prepare();
    unsigned wb_translations = translations;
    int nc_ready = a733_memory_uncached_prepare();
    unsigned nc_translations = translations - wb_translations;
    printf("translations wb=%u nc=%u total=%u\n", wb_translations, nc_translations, translations);
    struct device_memory_pool *wb = a733_memory_reservations_init(), *nc = a733_memory_uncached_init();
    assert(!wb == !wb_ready && !nc == !nc_ready);
    if (fault_block != UINT32_MAX) {
        assert(!wb || !nc);
        /* A refusal is kept and stops the boot: the boot preparation reports
         * it, and later calls translate nothing. */
        unsigned before = translations;
        assert(!a733_memory_local_prepare());
        assert(a733_memory_reservations_init() == wb && a733_memory_uncached_init() == nc);
        assert(translations == before);
        puts("PASS local preparation: a faulting block refuses its pool, once");
        return 0;
    }
    assert(wb && nc && wb != nc);
    assert(wb->backing.cpu == POOL_START && wb->backing.bytes == POOL_END - POOL_START);
    assert(nc->backing.cpu == 0x41c00000 && nc->backing.bytes == 0x200000);
#if defined(PREPARED_POOLS)
    /* Prepared pools: no class minimum, write-back and non-cacheable types. */
    assert(wb->memory_class == DEVICE_MEMORY_CLASS_NONE && wb->memory_type == DEVICE_MEMORY_WRITE_BACK);
    assert(nc->memory_class == DEVICE_MEMORY_CLASS_NONE && nc->memory_type == DEVICE_MEMORY_NON_CACHEABLE);
    unsigned before = translations;
    assert(a733_memory_reservations_init() == wb && a733_memory_uncached_init() == nc);
    assert(translations == before); /* Published pools make no further translations. */
    /* Preparation called again reports success and translates nothing. */
    assert(a733_memory_local_prepare() == 1 && translations == before);
#endif
    puts("PASS local preparation: identity write-back and non-cacheable pools published");
    return 0;
}
