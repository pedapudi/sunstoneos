#ifndef SUNSTONE_NPU_SESSION_H
#define SUNSTONE_NPU_SESSION_H
#include "a733_npu_application.h"
#define NPU_SESSION_ADDRESS UINT64_C(0x41f00000)
#define NPU_SESSION_MAGIC UINT64_C(0x415337334e504131)
#define NPU_CONTRACT_MAGIC UINT64_C(0x415337334e504331)
#ifdef SUNSTONE_NPU_SINGLE_CORE
#define NPU_SESSION_VERSION 3
struct a733_smp_contract;
int npu_single_core_seal(const struct a733_smp_contract *);
#else
#define NPU_SESSION_VERSION 2
#endif
#define NPU_WINDOW_TICKS (UINT64_C(20)*24000000)
/* Authenticated immutable page. The evidence digests bind reviewed records of
 * admission (platform and DDR), clock, power and supervision conditions;
 * nonzero fields do not prove them. Startup admits only PLL_NPU disabled and
 * coherent domain 4 OFF or ON, with local gates closed and engine resets asserted. */
struct npu_session {
    uint64_t magic,version,bytes,epoch,generation,lease;
    uint8_t image[32],evidence[32],clock[32],power[32],supervision[32];
    uint64_t supervisor,whole_platform;
    struct a733_npu_application_boot boot;
};
_Static_assert(sizeof(struct npu_session)<=4096,"immutable NPU session page");
_Static_assert(offsetof(struct npu_session,image)==48 &&
    offsetof(struct npu_session,supervisor)==208 && offsetof(struct npu_session,boot)==224,
    "supervised host session prefix");
extern const uint64_t a733_npu_session_layout[6];
int npu_session_valid(const struct npu_session *);
int npu_session_copy(void);
const struct npu_session *npu_session_get(void);
#endif
