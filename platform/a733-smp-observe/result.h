#ifndef SUNSTONE_A733_OBSERVE_RESULT_H
#define SUNSTONE_A733_OBSERVE_RESULT_H
#include <stddef.h>
#include <stdint.h>
#define A733_OBSERVE_FRAME_BYTES 512u
#define A733_OBSERVE_REQUEST_BYTES 64u
#define A733_OBSERVE_MESSAGE_MAGIC UINT32_C(0x53534f53)
#define A733_OBSERVE_COMMAND UINT32_C(0x31)
#define A733_OBSERVE_FRAME_MAGIC UINT64_C(0x415337334f425331)
/* Scalar boot copy precedes cache publication. Runtime readers never access
 * the Secure contract. All counters use the admitted 24 MHz physical clock. */
struct a733_observe_identity {
    uint64_t epoch,generation,lease,armed_at,deadline;
};
struct a733_observe_snapshot {
    uint64_t start,end,core[8][5]; /* rounds, background, SGIs, failure, ticks */
};
extern struct a733_observe_identity a733_observe_identity;
void a733_observe_boot_identity(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
void a733_observe_digest(const uint8_t *,size_t,uint8_t[32]);
/* Trusted fixed extents, independent buffers. No pointers come from USB. */
int a733_observe_request(const uint8_t[A733_OBSERVE_REQUEST_BYTES],
    const struct a733_observe_identity *,uint32_t *,uint8_t[32]);
int a733_observe_frame(uint8_t[A733_OBSERVE_FRAME_BYTES],
    const struct a733_observe_identity *,const struct a733_observe_snapshot *,
    const uint8_t[32],const uint8_t[32]);
void a733_observe_freeze(void);
void a733_observe_service_start(void);
void a733_observe_privileged(void);
void a733_observe_svc(void);
void a733_observe_deadline(void);
_Noreturn void a733_observe_failure(uint64_t);
#endif
