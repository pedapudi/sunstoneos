#ifndef SUNSTONE_PROBE_H
#define SUNSTONE_PROBE_H
#include <stddef.h>
#include <stdint.h>

#define SUNSTONE_PROBE_MAGIC UINT64_C(0x3145424f52505353)
#define SUNSTONE_PROBE_SESSION_MAGIC UINT64_C(0x3153455342505353)
#define SUNSTONE_PROBE_RESULT_MAGIC UINT64_C(0x3153455242505353)
#define SUNSTONE_PROBE_COMPLETE UINT64_C(0x454e4f4442505353)
#define SUNSTONE_PROBE_READY UINT64_C(0x5944414552505353)
#define SUNSTONE_PROBE_SESSION_ADDRESS UINT64_C(0x42004000)
#define SUNSTONE_PROBE_RESULT_ADDRESS UINT64_C(0x42000080)
#define SUNSTONE_PROBE_RETURNABLE 1u
#define SUNSTONE_PROBE_POWER_REMOVAL 2u
/* Set with RETURNABLE by an image whose SD core is built with SUNXI_MMC_READ_ONLY. The runner
 * refuses the image if it links any card write symbol or lacks sunxi_mmc_read_only_build. */
#define SUNSTONE_PROBE_STORAGE_READ_ONLY 4u
#define SUNSTONE_PROBE_WDT_MODE 0xb1u
struct sunstone_probe_marker {
    uint64_t magic,version,bytes,watchdog_mode,seconds;
    uint64_t session_address,session_bytes,result_address,result_bytes;
    uint64_t payload_address,payload_bytes,input_address,input_bytes,flags;
};
struct sunstone_probe_session {
    uint64_t magic,version,bytes,epoch,generation,lease,seconds,input_bytes,input_hash;
    uint64_t image[4],reserved[3];
};
struct sunstone_probe_result {
    uint64_t magic,version,bytes,epoch,generation,lease,status,payload_bytes,payload_hash;
    uint64_t cleanup,retired,start_ticks,end_ticks,failure_site,watchdog_mode,complete;
};
_Static_assert(sizeof(struct sunstone_probe_marker)==112,"probe marker ABI");
_Static_assert(sizeof(struct sunstone_probe_session)==128,"probe session ABI");
_Static_assert(sizeof(struct sunstone_probe_result)==128,"probe result ABI");
#define SUNSTONE_PROBE_IMAGE_FLAGS(payload,capacity,input,length,seconds_,flags_) \
    __attribute__((used,retain,section(".probe_marker"))) \
    const struct sunstone_probe_marker sunstone_probe_marker = { \
        SUNSTONE_PROBE_MAGIC,1,112,SUNSTONE_PROBE_WDT_MODE,seconds_, \
        SUNSTONE_PROBE_SESSION_ADDRESS,128,SUNSTONE_PROBE_RESULT_ADDRESS,128, \
        payload,capacity,input,length,flags_ }
#define SUNSTONE_PROBE_IMAGE(payload,capacity,input,length,seconds_) \
    SUNSTONE_PROBE_IMAGE_FLAGS(payload,capacity,input,length,seconds_,SUNSTONE_PROBE_RETURNABLE)
extern const struct sunstone_probe_marker sunstone_probe_marker;
/* Strong image-owned implementation. Retire DMA/IRQs/workers and restore
 * resources before returning READY. A failure must return another value. */
uint64_t sunstone_probe_quiesce(void);
/* Optional bounded EL3 fatal cutoff, called once after the first RTC fault is
 * published. Executes with MMU/caches off and IRQs masked. No RTOS, allocation,
 * waits or dependence on dirty Nonsecure EL1 data. Does not establish quiescence
 * or authorize a normal return. The default is empty. */
void sunstone_probe_abort_devices(void);
uint64_t sunstone_probe_restore_interrupts(void); /* EL3; strong monitor-owned hook */
/* EL3 hooks: default acquisition normalizes acknowledged secondary OFF to
 * static OFF with GIC wake disabled, retained through ROM. A strong owner
 * must meet the same held-state checks; release follows all cache work. */
uint64_t sunstone_probe_acquire_secondaries(void);
uint64_t sunstone_probe_release_secondaries(void);
void sunstone_probe_begin(void); /* EL3, before any whole-cache maintenance. */
_Noreturn void sunstone_probe_complete(unsigned status,size_t bytes); /* EL1 or EL3 */
_Noreturn void sunstone_probe_fault(uint64_t site,uint64_t esr,uint64_t elr,uint64_t far); /* EL3 */
_Noreturn void sunstone_probe_return(unsigned status); /* EL3, after owned IRQ restoration */
_Noreturn void sunstone_probe_timeout(void);
_Noreturn void sunstone_probe_fail(uint64_t site,uint64_t detail);
void sunstone_probe_progress_el3(void);
static inline void sunstone_probe_progress(void)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el==12){sunstone_probe_progress_el3();return;}
    register uint64_t op __asm__("x0")=0x734;
    __asm__ volatile("smc #0":"+r"(op)::"memory");
}
extern uint64_t sunstone_probe_deadline;
#endif
