#ifndef SUNSTONE_A733_DISPLAY_ENGINE_H
#define SUNSTONE_A733_DISPLAY_ENGINE_H
#include "a733_tcon_lcd.h"
#include <stdint.h>

#define A733_DE_BASE UINT32_C(0x05000000)
struct a733_de_surface {
    uint64_t device_address, mapped_bytes, owner;
    uint32_t pitch;
};
struct a733_de_plan {
    uint32_t overlay[64], blender[64], size;
    uint64_t padded_bytes, last_pixel_address;
};
/* Pure plan: fixed UCH3 layer0 -> DE1 pipe0 -> LCD1 -> DSI1, progressive
 * full-frame XRGB8888 (little-endian B,G,R,X), global alpha255, no scaling.
 * Pitch is bytes, device address is a 40-bit DMA address, not a CPU pointer.
 * The complete padded image must fit the supplied mapping and 40-bit range.
 * Native timing admission composes the existing LCD1/DSI1 planner. No clock
 * measurement, pin selection or framebuffer access occurs. Inputs/output
 * must be disjoint. Output is unchanged on rejection. */
int a733_de_plan(const struct a733_tcon_lcd_timing *, const struct a733_de_surface *, struct a733_de_plan *);

/* Additional closed profiles: UCH3 -> DE1 -> TV0 or TV1, 1280x720 RGB565.
 * The HDMI or DisplayPort controller independently establishes the exact
 * 720p60 timing. Both TV outputs use this one planner. */
enum a733_de_output { A733_DE_LCD1_XRGB8888=0, A733_DE_TV0_RGB565=1, A733_DE_TV1_RGB565=2 };
int a733_de_plan_tv0(const struct a733_de_surface *,struct a733_de_plan *);
enum a733_de_action { A733_DE_PREPARE, A733_DE_START, A733_DE_OBSERVE, A733_DE_STOP, A733_DE_REPLAY };
enum a733_de_state { A733_DE_PENDING, A733_DE_READY, A733_DE_ARMED,
    A733_DE_DISABLED_RETAINED, A733_DE_FAULT_RETAINED };
enum a733_de_error { A733_DE_OK, A733_DE_TIME, A733_DE_IO, A733_DE_AUTHORITY,
    A733_DE_INITIAL_STATE, A733_DE_VERIFY, A733_DE_REENTRY };
struct a733_de_config;
struct a733_de_ops {
    int (*admit)(void *, const struct a733_de_config *, enum a733_de_action);
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*write32)(void *, uintptr_t, uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *, uint64_t *);
    /* Optional replay: one non-preemptible busy-test/control-read/update-write.
     * 1 submitted,0 not busy (no write),-1 uncertain. No callback waits. */
    int (*rcq_submit)(void *);
};
struct a733_de_config {
    struct a733_de_ops ops;
    void *context;
    uint64_t lease;
    struct a733_tcon_lcd_timing timing;
    struct a733_de_surface surface;
    enum a733_de_output output; /* Zero preserves the original LCD1 profile. */
};
struct a733_de {
    struct a733_de_config config;
    struct a733_de_plan plan;
    uint64_t deadline, last_now;
    uint64_t replay_frame_bytes, replay_rcq_done, replay_frame_end;
    unsigned replay_slot;
    uint32_t expected, raw_status;
    enum a733_de_state state;
    enum a733_de_error error;
    enum a733_de_action action;
    unsigned initialized, entered, step, index, substep, writes_attempted;
    /* Ring slot of the last completed replay (slot 0 after START), the slot
     * count of the last accepted replay and completed replays. */
    unsigned scanout_slot, replay_slots;
    uint64_t presented;
};
/* Fresh zero storage; privileged serialized caller; no reinitialization.
 * Callbacks/context and driver storage remain private, live and disjoint.
 * Callbacks return exactly1, are bounded and non-reentrant. MMIO is Device
 * memory; barrier completes prior writes. Admission authenticates:
 * - lifetime exclusion over the entire DE, both outputs, all shared routing,
 *   line-buffer, urgency, offline/writeback/RCQ controls, and LCD1/DSI1 route;
 * - stable DE power, CCU/internal gates and released resets, resolved cold or
 *   reset handoff, independently recovered/quiescent DMA and no RCQ request;
 * - the immutable surface owner, mapping and full padded image visibility,
 *   address translation/IOMMU policy and hardware burst bounds, including
 *   any necessary mapped padding beyond the last row. No CPU pointer is DMA;
 * - PREPARE/START: the selected output is inactive/prepared with exact timing;
 *   TV0_RGB565 requires real 720p60 HDMI admission and retained DMA ownership;
 *   OBSERVE: coordinated feed and ownership; STOP: selected DSI1 or HDMI/TV0
 *   output already stopped while necessary DE clocks remain available. No other output/producer may start.
 * All memory and resource ownership remains held after STOP or any fault.
 * Only an external authority with independent transaction-drain evidence may
 * later reclaim the frame or gates. A disabled DE or frame-end is insufficient.
 * Production platform/clock/IOMMU binding remains a prerequisite. */
int a733_de_init(struct a733_de *, const struct a733_de_config *, uint64_t deadline);
/* READY->START; ARMED->OBSERVE/STOP. Absolute monotonic microsecond deadline.
 * No callback on refusal; no retry/restart/release or live frame update. */
int a733_de_request(struct a733_de *, enum a733_de_action, uint64_t deadline);
/* One admission/MMIO/barrier per poll with pre/post deadline checks.
 * PREPARE uses inactive AHB programming; START enables the producer for a
 * later authenticated LCD1 start. ARMED means programmed/read back, not that
 * pixels reached a panel. OBSERVE records raw status without acknowledgement.
 * REPLAY (see below) submits one RCQ update, waits for RCQ_FINISH, verifies
 * the layer address and then waits for a frame-end latched after a cleared
 * and read-back status. Interrupts stay disabled. No vblank accounting or
 * DMA-drain claim; a733_de_replay_writable() reports slots that left scanout. */
enum a733_de_state a733_de_poll(struct a733_de *);
/* Optional closed replay ring: [F0]..[Fn-1][4KiB RCQ0]..[4KiB RCQn-1] with
 * 2 <= n <= A733_DE_RING_SLOTS; the surface mapping is exactly n*(F+4096).
 * F is page aligned and covers the admitted padded surface. Command pages stay
 * immutable; the whole ring stays mapped READ and permanently owned. The closed
 * LCD1/XRGB8888, TV0/RGB565 and TV1/RGB565 profiles use the same DE1 queue.
 * The queue changes only UCH3 layer0 low address. A frame-end seen at the
 * post-clear readback is cleared again until the readback shows it clear,
 * bounded by the request deadline. The replay functions without "ring" in
 * their name build and check the original two-slot bank. */
#define A733_DE_RING_SLOTS 4u
int a733_de_ring_build(void *commands,uint64_t bank_iova,uint64_t frame_bytes,unsigned slots);
int a733_de_ring_valid(const void *commands,uint64_t bank_iova,uint64_t frame_bytes,unsigned slots);
int a733_de_replay_build(void *commands,uint64_t bank_iova,uint64_t frame_bytes);
int a733_de_replay_valid(const void *commands,uint64_t bank_iova,uint64_t frame_bytes);
int a733_de_replay_request(struct a733_de *,unsigned slot,uint64_t frame_bytes,uint64_t deadline);
/* 1 when the owner may rewrite this slot's pixels: the DE is ARMED with no
 * request pending, and the slot is not the one the last completed replay (or
 * START, for slot 0) selected. Completion requires RCQ finish, address
 * readback and a later frame-end, so the old slot is outside the current
 * frame's fetches. Only pixel contents are recycled; the ring stays mapped,
 * owned and retained. The writer publishes the slot (barrier for an uncached
 * mapping, clean to the point of coherency for a cached alias) before its
 * replay request. STOP, OBSERVE pending, any fault or a different geometry
 * returns 0; no transition after STOP makes a slot writable. */
int a733_de_replay_writable(const struct a733_de *,unsigned slot,uint64_t frame_bytes);
#endif
