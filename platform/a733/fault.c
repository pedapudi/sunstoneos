/* Previous-boot fault report and slot rotation (docs/fault-capture.md).
 *
 * a733_fault_boot runs once per boot on core 0 at EL3 with translation off,
 * where every access is a Device access: unaligned accesses and DC ZVA fault.
 * It therefore makes only aligned 64-bit volatile accesses and calls no
 * memset or memcpy (the image builds with -mstrict-align and
 * -mgeneral-regs-only). a733_cached_prepare publishes the written buffer to
 * EL1 afterwards with the rest of image RAM. */
#include "a733_fault.h"

uint64_t a733_fault_pending __attribute__((aligned(8)));
struct a733_fault_report a733_fault_previous __attribute__((aligned(64)));

/* Loops stay rolled: this runs once per boot, and the code budget is 1 KiB. */
#define ROLLED _Pragma("clang loop unroll(disable)")
__attribute__((target("crc"))) void a733_fault_boot(void)
{
    volatile const uint64_t *report = (volatile const uint64_t *)(uintptr_t)A733_FAULT_STAGE2_REPORT;
    volatile uint64_t *slots = (volatile uint64_t *)(uintptr_t)A733_FAULT_SLOTS_ADDRESS;
    volatile uint64_t *out = (volatile uint64_t *)&a733_fault_previous;
    uint64_t header = A733_FAULT_REPORT_MAGIC;
    /* The loader moved the previous resident result into its report before
     * clearing it, and published ENTERED for this boot. */
    int copied = report[0] == A733_FAULT_STAGE2_MAGIC && report[3] == A733_FAULT_STAGE2_ENTERED;
    if (copied) header |= A733_FAULT_REPORT_RESULT;
    ROLLED for (unsigned i = 0; i < 16; ++i)
        out[1 + i] = copied ? report[A733_FAULT_STAGE2_RESULT_WORD + i] : 0;
    ROLLED for (unsigned core = 0; core < A733_FAULT_CORES; ++core) {
        volatile uint64_t *s = slots + core * 16, *d = out + 17 + core * 16;
        uint32_t crc = UINT32_MAX;
        ROLLED for (unsigned i = 2; i < 16; ++i) crc = __builtin_arm_crc32cd(crc, s[i]);
        uint64_t magic = s[0];
        int valid = magic == A733_FAULT_SLOT_MAGIC && s[1] == (uint64_t)(uint32_t)~crc;
        if (valid) header |= UINT64_C(1) << (32 + core);
        else if (magic == A733_FAULT_SLOT_OPEN) header |= UINT64_C(1) << (40 + core);
        /* A valid slot is read a second time as it is copied; the others
         * read as zero in the report. */
        ROLLED for (unsigned i = 0; i < 16; ++i) {
            d[i] = valid ? s[i] : 0;
            s[i] = 0;
        }
    }
    out[0] = header;
}
