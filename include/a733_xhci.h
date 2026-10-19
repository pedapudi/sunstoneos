/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_XHCI_H
#define SUNSTONE_A733_XHCI_H
#include "device_memory_pool.h"
#include <stdint.h>

#define A733_XHCI_MEMORY_BYTES 131072u
#define A733_XHCI_TRANSFER_BYTES 4096u
#define A733_XHCI_RING_TRBS 16u
#define A733_XHCI_EVENT_TRBS 32u
#define A733_XHCI_ENDPOINTS 3u
#define A733_XHCI_DEVICES 3u
#define A733_XHCI_DMA_ENDPOINTS 7u

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
    unsigned kind,direction,length,aborted,transfer_type,interval,burst; /* pending:0 idle,1 queued,2 on ring. */
};
/* One root device and up to two children behind an admitted HS hub retain
 * own EP0, contexts and data rings; command/event rings remain controller-owned. */
struct a733_xhci_device {
    uint8_t *input,*device;
    uint64_t input_pa,device_pa,deadline;
    unsigned slot,address,pending_address,hardware_address,port_ready,speed;
    unsigned control_stage,control_length,control_in,evaluate_pending,address_status;
    unsigned configure_pending,command_mask,zero_retired,parent,parent_port;
    unsigned hub,hub_ports,hub_ttt,hub_mtt,hub_configured;
    uint8_t setup[8];
    struct a733_xhci_endpoint endpoints[A733_XHCI_ENDPOINTS];
};
/* Each device has EP0 and two bulk or interrupt endpoints. Only the owning
 * task calls this API. The private NC parent and every object stay at their
 * original addresses; no reinitialization or memory release exists.
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
    unsigned phase,failed,announced,removed,operational,runtime,doorbell;
    unsigned context_bytes,scratchpads,cap_slots,cap_ports,usb2_port,ext_cursor,ext_steps,speed;
    unsigned command_index,command_cycle,event_index,event_cycle,command_kind,command_device,default_device;
    unsigned port_reset,connected,extra_endpoint_owner,require_ss,usb3_port,usb3_slot_type,usb3_seen;
    unsigned root_u1,root_u2,usb2_companion_port;
    unsigned slot_type,publication,notifying,port_power_control;
    unsigned irq_mode,irq_service,irq_events,irq_asserted;
    uint64_t reset_deadline;
    struct a733_xhci_device devices[A733_XHCI_DEVICES];
};

/* Called after the actual USB2 platform is prepared, before publishing DMA
 * pointers. The pre-scheduler constructor supplies its own CPU-state NC pool
 * lease; init validates it and permanently transfers the parent to DEVICE.
 * Capability limits are observed before populating any hardware context.
 * Initial support uses standard USB2 protocol speed IDs, one direct high-speed
 * device, directly attached FS/LS HID, or one HS hub with the admitted leaves.
 * Streams, deeper hubs and isochronous endpoints remain unsupported.
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
 * can be bound before the parent hub Configure Endpoint completion. */
int a733_xhci_bind_child(struct a733_xhci *,unsigned parent,unsigned port,unsigned speed,uint64_t deadline);
int a733_xhci_mark_hub(struct a733_xhci *,unsigned address);
int a733_xhci_hub_configure(struct a733_xhci *,unsigned address,unsigned ports,unsigned ttt,unsigned mtt);
int a733_xhci_retire_zero(struct a733_xhci *);
unsigned a733_xhci_device_speed(const struct a733_xhci *,unsigned address);
int a733_xhci_reset_port(struct a733_xhci *,uint64_t deadline);
int a733_xhci_port_connected(const struct a733_xhci *);
int a733_xhci_open(struct a733_xhci *,unsigned address,unsigned endpoint,
    unsigned transfer_type,unsigned packet,uint64_t deadline);
/* Interrupt bInterval is the raw USB descriptor value. Only single-transaction
 * interrupt IN packets are admitted, <=8 bytes LS and <=64 bytes FS/HS. */
int a733_xhci_set_burst(struct a733_xhci *,unsigned address,unsigned endpoint,unsigned burst);
int a733_xhci_open_interrupt(struct a733_xhci *,unsigned address,unsigned endpoint,
    unsigned packet,unsigned interval,uint64_t deadline);
int a733_xhci_setup(struct a733_xhci *,unsigned address,const uint8_t[8],uint64_t deadline);
int a733_xhci_transfer(struct a733_xhci *,unsigned address,unsigned endpoint,
    uint8_t *,unsigned bytes,uint64_t deadline);
/* Immediately revoke caller destinations. Aborting an actual pending operation
 * permanently retains the complete session; no Stop Endpoint/reclamation path
 * is implied. Absent or idle endpoints have nothing to cancel and return1.
 * Private storage and slot ownership never return to the pool, even on success.
 */
int a733_xhci_abort(struct a733_xhci *,unsigned address,unsigned endpoint);
void a733_xhci_lost(struct a733_xhci *);
#endif
