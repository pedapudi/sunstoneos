/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_POWER_DOMAINS_NATIVE_H
#define SUNSTONE_A733_POWER_DOMAINS_NATIVE_H
#include "a733_power_domains.h"
/* Device accessors and the service-task wake for the power-domain owner.
 * Reads cover the policy, status and delay-row words of PPU domains 1, 4 and
 * 5, CCU 0x5c0, 0x5e0, 0xb20 and 0xb24 and R_CCU 0x1ac; writes cover only the
 * policy and delay-row words and only from the service task. Every access is
 * a733_mmio_*_dsb_sy_both. The first access verifies that each window is an
 * identity Device-nGnRnE mapping; a failed check refuses every later access. */
struct a733_power_domains_native {
    struct a733_power_domain_ops operations;
    void *task; /* the service task, also the PMIC provider task */
    unsigned initialized, mapped;
};
/* Before the scheduler, with the created service task. */
int a733_power_domains_native_init(struct a733_power_domains_native *,void *task);
/* Pure allowlist, shared with checks/power-domains-tree.py. */
static inline int a733_power_domains_native_address(uintptr_t a,unsigned writing)
{
    if(a&3)return 0;
    if(a>=UINT32_C(0x07061000) && a<UINT32_C(0x07066000)) {
        const uintptr_t block=(a-UINT32_C(0x07060000))>>12,offset=a&UINT32_C(0xfff);
        if(block!=1 && block!=4 && block!=5)return 0;
        return offset==0 || offset==0x170 || offset==0x174 || offset==0xc00 || offset==0xc04 ||
            offset==0xc10 || (!writing && offset==8);
    }
    return !writing && (a==UINT32_C(0x020025c0) || a==UINT32_C(0x020025e0) || a==UINT32_C(0x02002b20) ||
        a==UINT32_C(0x02002b24) || a==UINT32_C(0x070101ac));
}
#endif
