#include "umem_cpu.h"
#include "aarch64_device_memory.h"

static void store(void *ctx, uint64_t *entry, uint64_t value) { (void)ctx; *(volatile uint64_t *)entry = value; }
static void dsb_ish(void *ctx) { (void)ctx; __asm__ volatile("dsb ish" ::: "memory"); }
static void isb(void *ctx) { (void)ctx; __asm__ volatile("isb" ::: "memory"); }
static void tlbi_va_all_asids(void *ctx, uint64_t va) { (void)ctx; __asm__ volatile("tlbi vaae1is, %0" :: "r"(va >> 12) : "memory"); }
static int dc_ivac(void *ctx, uint64_t va, uint64_t bytes)
{
    (void)ctx;
    return aarch64_device_cache(AARCH64_CACHE_INVALIDATE, (uintptr_t)va, (size_t)bytes) ? 0 : -1;
}

const struct umem_cpu_ops umem_cpu_el1 = {0, store, dsb_ish, isb, tlbi_va_all_asids, dc_ivac};

int umem_maintain_el1(void *ctx, enum umem_op op, uint64_t base, uint64_t bytes)
{
    (void)ctx;
    switch (op) {
    case UMEM_OP_ZERO:
        for (uint64_t a = base; a < base + bytes; a += 8) *(volatile uint64_t *)(uintptr_t)a = 0;
        return 0;
    case UMEM_OP_CLEAN:
        return aarch64_device_cache(AARCH64_CACHE_CLEAN, (uintptr_t)base, (size_t)bytes) ? 0 : -1;
    case UMEM_OP_CLEAN_INVALIDATE:
        return aarch64_device_cache(AARCH64_CACHE_CLEAN_INVALIDATE, (uintptr_t)base, (size_t)bytes) ? 0 : -1;
    case UMEM_OP_STORE_BARRIER:
        __asm__ volatile("dmb oshst" ::: "memory");
        return 0;
    case UMEM_OP_LOAD_BARRIER:
        __asm__ volatile("dmb oshld" ::: "memory");
        return 0;
    }
    return -1;
}
