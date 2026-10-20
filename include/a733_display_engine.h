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

/* Additional closed profile: UCH3 -> DE1 -> TV0, 1280x720 RGB565.
 * The HDMI controller independently establishes the exact 720p60 timing. */
enum a733_de_output { A733_DE_LCD1_XRGB8888=0, A733_DE_TV0_RGB565=1, A733_DE_TV1_RGB565=2 };
/* TV1 uses the same immutable surface format and fixed720p60 dimensions. */
int a733_de_plan_tv1(const struct a733_de_surface *,struct a733_de_plan *);
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
 * No RCQ, interrupts, vblank accounting, DMA-drain claim or buffer retirement. */
enum a733_de_state a733_de_poll(struct a733_de *);
/* Optional closed two-frame replay bank: [F0][F1][4KiB RCQ0][4KiB RCQ1].
 * F is page aligned and covers the admitted padded surface. Both frames and
 * command pages stay immutable, mapped READ and permanently owned. Both closed
 * LCD1/XRGB8888 and TV0/RGB565 profiles use the same DE1 queue. The queue
 * changes only UCH3 layer0 low address; it grants no retirement or reuse. */
int a733_de_replay_build(void *commands,uint64_t bank_iova,uint64_t frame_bytes);
int a733_de_replay_valid(const void *commands,uint64_t bank_iova,uint64_t frame_bytes);
int a733_de_replay_request(struct a733_de *,unsigned slot,uint64_t frame_bytes,uint64_t deadline);
#endif
