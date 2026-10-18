#ifndef SUNSTONE_A733_DMA_COPY_APPLICATION_H
#define SUNSTONE_A733_DMA_COPY_APPLICATION_H
#include "a733_dma_pool_session.h"
/* Platform inputs to the dedicated copy application image. Supply a qualified
 * immutable handoff with a future absolute startup deadline. There is no default
 * authority record. This image owns its one common counter/registry/lock set;
 * a larger application instead borrows its existing set through the reusable
 * a733_dma_pool_session_init constructor. */
extern const struct a733_dma_handoff a733_dma_copy_handoff;
/* Separate immutable external USB0 reservation. Every assertion requires actual
 * current-boot evidence. These fields do not establish ownership or recovery.
 * The USB service exclusively owns USB0, its PHY and the complete CCU words at
 * 0x02003300/0x02003304 until reset; all other users remain stopped. */
#define A733_DMA_COPY_USB_REQUIRED UINT64_C(15)
struct a733_dma_copy_usb_handoff {
    uint64_t epoch,generation,lease,evidence,dma_evidence,dma_handoff,established;
};
extern const struct a733_dma_copy_usb_handoff a733_dma_copy_usb_handoff;
/* Flags: exclusive USB0/PHY/clock ownership, device mapping, remote-user
 * exclusion and independent watchdog/ROM recovery, respectively. */
#define A733_DMA_COPY_COMMAND UINT32_C(0x32)
#define A733_DMA_COPY_FRAME_MAGIC UINT64_C(0x41533733444d4131)
#define A733_DMA_COPY_BYTES 1024u
struct a733_observe_identity;
int a733_dma_copy_observer_admit(void);
int a733_dma_copy_request(const uint8_t[64],const struct a733_observe_identity *,
    uint32_t *,uint8_t[32]);
int a733_dma_copy_frame(uint8_t[512],const struct a733_observe_identity *,
    const uint8_t[32],const uint8_t[32]);
/* Publish timestamps and the final channel mask before release-storing complete.
 * The USB encoder acquires complete before reading these fields. */
struct a733_dma_copy_result {
    atomic_uint completed_channels;
    atomic_uint complete;
    uint64_t start,end;
};
extern struct a733_dma_copy_result a733_dma_copy_result;
/* Existing cached platform guard: primary EL1, all cores at the pre-scheduler
 * barrier, caches adopted, deadline valid, scheduler release not published. */
int a733_smp_device_preparation_admitted(void);
/* Cached application entry selected instead of the scheduler diagnostic. */
void a733_smp_diagnostic_start(void);
#endif
