/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_XHCI_H
#define SUNSTONE_A733_XHCI_H
#include "device_memory_pool.h"
#include <stdint.h>

#ifndef SUNSTONE_USB_HOST_GENERIC
#define SUNSTONE_USB_HOST_GENERIC 0
#endif
#define A733_XHCI_TRANSFER_BYTES 4096u
#define A733_XHCI_RING_TRBS 16u
#define A733_XHCI_EVENT_TRBS 32u
#define A733_XHCI_HUB_TIERS 2u /* Hub tiers below the root port. */
#define A733_XHCI_ISOCH_TDS 8u
#define A733_XHCI_DMA_BUFFERS 1u /* Legacy bounce page per endpoint. */
#if SUNSTONE_USB_HOST_GENERIC
#define A733_XHCI_MEMORY_BYTES 5242880u
#define A733_XHCI_ENDPOINTS 4u
#define A733_XHCI_DEVICES 6u
#define A733_XHCI_DMA_ENDPOINTS (A733_XHCI_DEVICES*A733_XHCI_ENDPOINTS)
#define A733_XHCI_ISOCH_PACKET 4096u /* Page stride; service payload <=3072. */
#define A733_XHCI_ISOCH_BUFFERS 1024u
#define A733_XHCI_DATA_OFFSET 262144u
#define A733_XHCI_DATA_BYTES (A733_XHCI_ISOCH_BUFFERS*A733_XHCI_ISOCH_PACKET)
#else
#define A733_XHCI_MEMORY_BYTES 131072u
#define A733_XHCI_ENDPOINTS 3u
#define A733_XHCI_DEVICES 3u
#define A733_XHCI_DMA_ENDPOINTS 7u
#define A733_XHCI_ISOCH_PACKET 512u
#define A733_XHCI_ISOCH_BUFFERS 8u /* Metadata for the legacy single bounce page. */
#define A733_XHCI_DATA_OFFSET 0u
#define A733_XHCI_DATA_BYTES 0u /* No public DMA leases in the legacy profile. */
#endif

enum a733_xhci_result { A733_XHCI_PENDING,A733_XHCI_READY,A733_XHCI_BUSY,
    A733_XHCI_INVALID,A733_XHCI_RETAINED };
enum a733_xhci_event { A733_XHCI_ATTACH,A733_XHCI_REMOVE,A733_XHCI_TRANSFER };
struct a733_xhci_notice {
    enum a733_xhci_event kind;
    uint64_t generation;
    unsigned address,endpoint,bytes,result; /* result0 success,1 stall,2 failure. */
};
struct a733_xhci_ops {
    int (*now)(void *,uint64_t *);
    int (*current)(void *);
    /* Actual platform and current Type-C generation:1 live,0 busy,-1 lost.
     * Never substitute a constant authorization callback in a native image. */
    int (*live)(void *,uint64_t generation,uint64_t deadline);
    int (*read32)(void *,uint32_t,uint32_t *);
    int (*write32)(void *,uint32_t,uint32_t);
    int (*write64)(void *,uint32_t,uint64_t); /* Defined xHCI pointer registers only. */
    int (*barrier)(void *); /* Ordered coherent NC DMA visibility. */
    int (*outside)(void *,const void *,size_t);
    void (*notice)(void *,const struct a733_xhci_notice *);
};
struct a733_xhci_trb { uint32_t parameter_lo,parameter_hi,status,control; };
struct a733_xhci_endpoint {
    struct a733_xhci_trb *ring;
    uint8_t *bounce,*caller;
    uint64_t ring_pa,bounce_pa,pending_trb,deadline;
    unsigned number,address,packet,index,cycle,opened,configured,pending;
    unsigned dma_active,dma_offset,dma_shared;
    unsigned kind,direction,length,aborted,transfer_type,interval,burst; /* pending:0 idle,1 queued,2 on ring,3 reset recovery,4 dequeue recovery,5 halted by transaction error,6 isochronous TDs on ring,7 awaiting bulk clear,8 stop idle bulk,9 configure idle bulk. */
};
/* One root device and up to five hub descendants retain own EP0, contexts and
 * data rings; command/event rings remain controller-owned. A child of a
 * USB2 hub at tier 1 sits at tier 2; parent is the parent hub's slot and
 * route the xHCI route string (one port nibble per tier). */
struct a733_xhci_device {
    uint8_t *input,*device;
    uint64_t input_pa,device_pa,deadline;
    unsigned slot,address,pending_address,hardware_address,port_ready,speed;
    unsigned control_stage,control_length,control_in,evaluate_pending,address_status;
    unsigned configure_pending,command_mask,zero_retired,parent,parent_port,route,tier;
    unsigned hub,hub_ports,hub_ttt,hub_mtt,hub_configured;
    unsigned read_recovery; /* One bulk STALL admitted by a mounted READ10 owner. */
    unsigned recovering; /* Endpoint index+1 while reset/clear/dequeue is serialized. */
    unsigned detach,stopping; /* detach:1 stopping endpoints,2 disabling slot; stopping:endpoint index+1. */
    uint8_t setup[8];
    struct a733_xhci_endpoint endpoints[A733_XHCI_ENDPOINTS];
};
/* One isochronous IN stream per controller. Entries head..head+count-1
 * (modulo A733_XHCI_ISOCH_TDS) are in TD order; the first done entries have
 * completed and the rest are on the ring. buffer[i] selects a shared NC page.
 * state: 1 on ring, 2 data, 3 no data. skip allows missed-service TD skips.
 * The shared page ledger also serves bulk/interrupt data leases. */
struct a733_xhci_isoch {
    unsigned device,requested,head,count,done,skip;
    unsigned bytes[A733_XHCI_ISOCH_TDS],state[A733_XHCI_ISOCH_TDS];
    uint64_t trb[A733_XHCI_ISOCH_TDS];
    unsigned buffer[A733_XHCI_ISOCH_TDS];
    uint64_t token[A733_XHCI_ISOCH_BUFFERS];
    unsigned held[A733_XHCI_ISOCH_BUFFERS],owner[A733_XHCI_ISOCH_BUFFERS],references[A733_XHCI_ISOCH_BUFFERS];
    uint32_t dropped,overruns; /* Intervals without data; empty-ring reports. */
};
/* Each device has EP0 and three bulk or interrupt endpoints. Only the owning
 * task calls this API. The private NC parent and every object stay at their
 * original addresses and the parent is never released. A device record's own
 * pages are cleared for reuse only after its Disable Slot completion.
 * Control stages use independent actual Setup/Data/Status TD completions.
 */
struct a733_xhci {
    struct a733_xhci *self;
    struct a733_xhci_ops ops;
    void *context;
    struct device_memory_pool *pool;
    struct device_memory_lease memory;
    struct device_memory_extent extent;
    struct a733_xhci_trb *commands,*events;
    uint8_t *dcbaa,*erst,*scratch;
    uint64_t command_pa,event_pa,dcbaa_pa,erst_pa;
    uint64_t generation,deadline,lifetime_deadline,last_now,command_deadline,command_pointer;
    unsigned phase,failed,announced,removed,detaching,operational,runtime,doorbell;
    unsigned context_bytes,scratchpads,cap_slots,cap_ports,usb2_port,ext_cursor,ext_steps,speed;
    unsigned command_index,command_cycle,event_index,event_cycle,command_kind,command_device,default_device;
    unsigned port_reset,connected,extra_endpoint_owner,require_ss,usb3_port,usb3_slot_type,usb3_seen;
    unsigned root_u1,root_u2,usb2_companion_port,companion_root;
    unsigned slot_type,publication,notifying,port_power_control;
    unsigned irq_mode,irq_service,irq_events,irq_asserted;
    uint64_t reset_deadline;
    struct a733_xhci_device devices[A733_XHCI_DEVICES];
    struct a733_xhci_isoch isoch;
};

/* Called after the actual USB2 platform is prepared, before publishing DMA
 * pointers. The pre-scheduler constructor supplies its own CPU-state NC pool
 * lease; init validates it and permanently transfers the parent to DEVICE.
 * Capability limits are observed before populating any hardware context.
 * Support uses standard USB2 protocol speed IDs, one direct device, or up to
 * two tiers of USB2 high-speed hubs (one SuperSpeed hub tier) with the
 * admitted leaves, and one isochronous IN stream. Streams remain unsupported.
 * Callbacks are bounded and non-reentrant; notice may enqueue a notification
 * but must not call any core API. All scalar offsets are relative to xHCI.
 */
int a733_xhci_init(struct a733_xhci *,const struct a733_xhci_ops *,void *,
    struct device_memory_pool *,struct device_memory_lease,uint64_t generation,
    uint64_t deadline,uint64_t lifetime_deadline);
/* At most one event and one state transition per call. Software deadlines do
 * not recover a stalled peripheral/interconnect; qualified recovery is required.
 */
enum a733_xhci_result a733_xhci_poll(struct a733_xhci *);
/* Owner-task scheduling hint: a software-only startup or queued request step
 * can run now. Metadata only; false does not establish hardware quiescence.
 * Hardware waits, teardown, recovery and isochronous refill keep their existing
 * scheduling. The caller must bound its batch and still run poll's guards. */
int a733_xhci_work_pending(const struct a733_xhci *);
/* Metadata only; owner must serialize access. Includes ISO inactivity, which
 * is checked only after queued events have been drained by poll. */
uint64_t a733_xhci_next_deadline(const struct a733_xhci *);
/* Optional owner-task interrupt profile. The platform keeps its level SPI
 * masked and deactivated throughout begin/drain/finish. Start returns0 until
 * ACTIVE, then1; all three return-1 and retain on failure. Begin acknowledges
 * EINT before IP. Poll still advances timers/submission without an interrupt,
 * but consumes events only inside a service. Finish returns0 for more drain,
 * 1 after empty/EHB release and race recheck, permitting platform rearm.
 * No ISR calls these functions. No drain or DMA release is implied. */
int a733_xhci_interrupt_start(struct a733_xhci *);
int a733_xhci_interrupt_begin(struct a733_xhci *);
int a733_xhci_interrupt_finish(struct a733_xhci *);
/* Owner supplies actual TinyUSB bus topology after hub reset/status. No child
 * can be bound before the parent hub Configure Endpoint completion. parent is
 * the hub's logical address; a hub at tier 1 must be a USB2 high-speed hub
 * below a high-speed hub. */
int a733_xhci_bind_child(struct a733_xhci *,unsigned parent,unsigned port,unsigned speed,uint64_t deadline);
int a733_xhci_mark_hub(struct a733_xhci *,unsigned address);
int a733_xhci_hub_configure(struct a733_xhci *,unsigned address,unsigned ports,unsigned ttt,unsigned mtt);
int a733_xhci_retire_zero(struct a733_xhci *);
unsigned a733_xhci_device_speed(const struct a733_xhci *,unsigned address);
int a733_xhci_reset_port(struct a733_xhci *,uint64_t deadline);
int a733_xhci_port_connected(const struct a733_xhci *);
int a733_xhci_open(struct a733_xhci *,unsigned address,unsigned endpoint,
    unsigned transfer_type,unsigned packet,uint64_t deadline);
/* Interrupt bInterval is the raw USB descriptor value. Single-transaction
 * IN/OUT packets are admitted, <=8 bytes LS and <=64 bytes FS/HS. */
int a733_xhci_set_burst(struct a733_xhci *,unsigned address,unsigned endpoint,unsigned burst);
int a733_xhci_open_interrupt(struct a733_xhci *,unsigned address,unsigned endpoint,
    unsigned packet,unsigned interval,uint64_t deadline);
/* Isochronous IN for a USB2 full- or high-speed device with a service interval
 * of at most 1 ms: FS bInterval 1, HS bInterval 1-4. One stream per
 * controller uses its own data ring and shared retained page pool. packet is
 * raw wMaxPacketSize: up to 1023 FS bytes or three 1024-byte HS transactions. */
int a733_xhci_open_isoch(struct a733_xhci *,unsigned address,unsigned endpoint,
    unsigned packet,unsigned interval,uint64_t deadline);
/* Start keeping A733_XHCI_ISOCH_TDS one-service-interval TDs (Start Isoch ASAP) on the
 * ring once the endpoint is configured. When a poll finds the event ring empty
 * 100 ms or more after the previous ring activity while TDs are queued, the
 * session is retained. Events posted while the owner did not poll are
 * consumed first, so a late poll alone does not retain the session. */
int a733_xhci_isoch_start(struct a733_xhci *,unsigned address,unsigned endpoint);
/* Copy the oldest completed packet into caller storage and free its entry;
 * the next poll queues a replacement TD. Entries that carried no data are
 * skipped and counted. Returns1 with *bytes set, 0 when none completed,
 * -1 on refusal. No DMA address reaches the caller. */
int a733_xhci_isoch_take(struct a733_xhci *,unsigned address,unsigned endpoint,
    uint8_t *out,unsigned capacity,unsigned *bytes);
/* Owner-task leases of retained coherent NC pages. Acquire returns 1, 0 when
 * full and -1 on refusal. Submit with transfer using exactly the acquired
 * pointer. A completion notice returns its page to the caller; another page
 * may then be submitted while earlier completed pages remain held. Release
 * refuses a page still on hardware. Tokens never survive a new generation.
 * Detach disables the slot, then defers record/page reuse until leases release.
 * An OUT transfer may use a completed same-device IN page plus a byte offset;
 * release refuses that source page until every OUT reference completes.
 * The parent is permanently retained even after all releases. */
int a733_xhci_dma_acquire(struct a733_xhci *,unsigned address,unsigned endpoint,
    uint8_t **data,unsigned *capacity,uint64_t *token);
int a733_xhci_dma_release(struct a733_xhci *,unsigned address,unsigned endpoint,uint64_t token);
/* Completed ISO service intervals are leased separately from the eight queued
 * TDs. Up to 1024 page leases may be held by frame consumers. Release returns
 * the page to the free pool; a later poll reuses it. Dropped intervals carry
 * no lease. All returned pointers expire on session loss. */
int a733_xhci_isoch_acquire(struct a733_xhci *,unsigned address,unsigned endpoint,
    const uint8_t **data,unsigned *bytes,uint64_t *token);
int a733_xhci_isoch_release(struct a733_xhci *,unsigned address,unsigned endpoint,uint64_t token);
int a733_xhci_setup(struct a733_xhci *,unsigned address,const uint8_t[8],uint64_t deadline);
int a733_xhci_transfer(struct a733_xhci *,unsigned address,unsigned endpoint,
    uint8_t *,unsigned bytes,uint64_t deadline);
/* Already configured bulk/interrupt endpoint: publish and ring directly in
 * caller context under the native execution gate. No enumeration or command
 * work is advanced. One outstanding transfer per endpoint remains the bound. */
int a733_xhci_transfer_now(struct a733_xhci *,unsigned address,unsigned endpoint,
    uint8_t *,unsigned bytes,uint64_t deadline);
/* Arm one matched bulk STALL for the mounted read-only storage consumer.
 * Requires an idle configured bulk pair; a second stall is not armed by retry. */
int a733_xhci_read_recovery(struct a733_xhci *,unsigned address);
/* After successful device CLEAR_FEATURE, restore one known idle/stalled bulk
 * endpoint. Completion is asynchronous; its pending/recovering state clears only
 * after validated command completions. No allocation or DMA release occurs. */
int a733_xhci_clear_bulk(struct a733_xhci *,unsigned address,unsigned endpoint,uint64_t deadline);
/* Immediately revoke caller destinations. Aborting an actual pending operation
 * permanently retains the complete session; use detach to end a device.
 * Absent, idle or detaching endpoints have nothing to cancel and return1.
 * Private storage never returns to the pool, even on success.
 */
int a733_xhci_abort(struct a733_xhci *,unsigned address,unsigned endpoint);
/* Orderly removal of one hub child and, for a hub, every device below it,
 * for example after its hub port reports a disconnect. Children are torn
 * down before their hub. Revokes caller destinations at once; later polls stop each
 * Running endpoint, disable the slot and then clear the record's pages for a
 * new child. Every step needs its matched completion within 5 s; otherwise the
 * session is retained. After Disable Slot, application leases may delay record
 * reuse without a hardware timeout. Returns1 when accepted, already in progress or nothing
 * is bound, and0 on refusal. The root device is torn down only after its port
 * reports a connect-status change; a REMOVE notice with result0 starts that,
 * and the next ATTACH follows its completion. A Type-C generation change still
 * retains the whole session. */
int a733_xhci_detach(struct a733_xhci *,unsigned address);
void a733_xhci_lost(struct a733_xhci *);
#endif
