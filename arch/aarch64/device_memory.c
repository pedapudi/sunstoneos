#include "aarch64_device_memory.h"

unsigned aarch64_device_cache_stride(void)
{
    uint64_t level, control, ctr;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(level));
    if (level != 4) return 0;
    __asm__ volatile("mrs %0, sctlr_el1\n mrs %1, ctr_el0" : "=r"(control), "=r"(ctr));
    if ((control & 5) != 5) return 0;
    return aarch64_cache_stride_from_ctr(ctr);
}

/* One AT form with DAIF masked and PAR_EL1 saved and restored; ISB orders
 * the PAR_EL1 read after the translation. */
#define TRANSLATE(operation) \
    __asm__ volatile("mrs %0, daif\n msr daifset, #0xf\n mrs %1, par_el1\n " \
                     operation ", %3\n isb\n mrs %2, par_el1\n msr par_el1, %1\n msr daif, %0" \
                     : "=&r"(saved_flags), "=&r"(saved_par), "=&r"(par) : "r"(address) : "memory")
static inline __attribute__((always_inline)) int decode(uint64_t par, uintptr_t address,
                                                         struct aarch64_translation *result)
{
    if (par & 1) return 0;
    *result = (struct aarch64_translation){
        (par & UINT64_C(0x000ffffffffff000)) | (address & 4095),
        (unsigned)(par >> 56), (unsigned)(par >> 7) & 3
    };
    return 1;
}

int aarch64_device_translate(uintptr_t address, int write, struct aarch64_translation *result)
{
    if (!result || !aarch64_device_cache_stride()) return 0;
    uint64_t saved_flags, saved_par, par;
    if (write) { TRANSLATE("at s1e1w"); }
    else { TRANSLATE("at s1e1r"); }
    return decode(par, address, result);
}

int aarch64_device_translate_el0(uintptr_t address, int write, struct aarch64_translation *result)
{
    if (!result || !aarch64_device_cache_stride()) return 0;
    uint64_t saved_flags, saved_par, par;
    if (write) { TRANSLATE("at s1e0w"); }
    else { TRANSLATE("at s1e0r"); }
    return decode(par, address, result);
}
#undef TRANSLATE

int aarch64_device_cache(enum aarch64_cache_op operation, uintptr_t cpu, size_t bytes)
{
    unsigned stride = aarch64_device_cache_stride();
    if (!stride || !cpu || !bytes || (cpu & 63) || (bytes & 63) || cpu > UINTPTR_MAX - bytes ||
        (operation != AARCH64_CACHE_CLEAN && operation != AARCH64_CACHE_CLEAN_INVALIDATE &&
         operation != AARCH64_CACHE_INVALIDATE)) return 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uintptr_t end = cpu + bytes;
    for (uintptr_t address = cpu; address < end; address += stride) {
        if (operation == AARCH64_CACHE_CLEAN)
            __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
        else if (operation == AARCH64_CACHE_CLEAN_INVALIDATE)
            __asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
