#ifndef SUNSTONE_NPU_SESSION_H
#define SUNSTONE_NPU_SESSION_H
#include "a733_npu_application.h"
#define NPU_SESSION_ADDRESS UINT64_C(0x41f00000)
#define NPU_SESSION_MAGIC UINT64_C(0x415337334e504131)
#define NPU_CONTRACT_MAGIC UINT64_C(0x415337334e504331)
#define NPU_WINDOW_TICKS (UINT64_C(20)*24000000)
/* Authenticated immutable page; clock evidence includes independent DRAM/PHY
 * sources, all PLL_NPU consumers/writers, and the complete snapshot below.
 * Evidence binds physical prerequisites; nonzero fields do not prove them. */
struct npu_session {
    uint64_t magic,version,bytes,epoch,generation,lease;
    uint8_t image[32],evidence[32],clock[32],power[32],supervision[32];
    uint64_t supervisor,whole_platform;
    struct a733_npu_application_boot boot;
    struct a733_npu_pll_config pll;
};
_Static_assert(sizeof(struct npu_session)<=4096,"immutable NPU session page");
_Static_assert(offsetof(struct npu_session,image)==48 &&
    offsetof(struct npu_session,supervisor)==208 && offsetof(struct npu_session,boot)==224,
    "supervised host session prefix");
extern const uint64_t a733_npu_session_layout[8];
int npu_session_valid(const struct npu_session *);
int npu_session_copy(void);
const struct npu_session *npu_session_get(void);
#endif
