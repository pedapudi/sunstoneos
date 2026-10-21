/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_CAMERA_DMA_H
#define SUNSTONE_A733_CAMERA_DMA_H
#include <stdint.h>
#include "a733_camera_common.h"

/* One serialized privileged owner, fresh zeroed storage, no callback reentry.
 * One NV16 frame: separate Y and interleaved UV planes, stride width, each
 * width x height bytes. a733_camera_dma_init selects 640x480.
 * This is a DMA140 component; route/VIPP and single-frame gating are required.
 */
enum a733_camera_dma_state {
    A733_CAM_DMA_NEW, A733_CAM_DMA_ADMIT, A733_CAM_DMA_CHECK,
    A733_CAM_DMA_CONFIGURE, A733_CAM_DMA_ARMED, A733_CAM_DMA_START,
    A733_CAM_DMA_WAIT, A733_CAM_DMA_STOP, A733_CAM_DMA_DISABLE,
    A733_CAM_DMA_DRAIN, A733_CAM_DMA_FINAL_STATUS, A733_CAM_DMA_SYNC,
    A733_CAM_DMA_READY, A733_CAM_DMA_RELEASE, A733_CAM_DMA_CLOSED,
    A733_CAM_DMA_FAULT, A733_CAM_DMA_READABLE, A733_CAM_DMA_REARM,
    A733_CAM_DMA_STREAM_START, A733_CAM_DMA_STREAMING
};
/* Continuous capture writes a ring of 2..A733_CAMERA_DMA_SLOTS frame slots.
 * Slot k's planes start k * slot_bytes into the Y and UV spans. */
#define A733_CAMERA_DMA_SLOTS 4u
enum a733_camera_slot {
    A733_SLOT_FREE,   /* Device-owned; no address register names it. */
    A733_SLOT_NEXT,   /* Programmed; the next frame start latches it. */
    A733_SLOT_ACTIVE, /* Latched; the device writes the current frame. */
    A733_SLOT_SYNC,   /* Complete; awaiting CPU cache invalidation. */
    A733_SLOT_READY,  /* Complete and visible; queued for the consumer. */
    A733_SLOT_HELD    /* Taken by the consumer for reading. */
};
struct a733_camera_ring {
    uint64_t slot_bytes;
    /* Frame starts observed, frames delivered, and frames lost: overwritten
     * because no free slot existed, incomplete without a frame-done
     * observation, or unattributable after an address write that may have
     * raced a frame start. */
    uint32_t starts, delivered, dropped;
    uint32_t sequence[A733_CAMERA_DMA_SLOTS]; /* Frame start count per slot. */
    uint8_t state[A733_CAMERA_DMA_SLOTS], queue[A733_CAMERA_DMA_SLOTS];
    uint8_t count, active, next, queued;
    /* A frame-done observation followed the active frame's start, and that
     * start is known to have latched the active slot. */
    uint8_t frame_done, trusted;
};
enum a733_camera_frame_owner {
    A733_CAMERA_CALLER, A733_CAMERA_RESERVED, A733_CAMERA_DEVICE
};
struct a733_camera_plane {
    uint64_t bus_address;
    /* Full mapped span, including any platform-required tail/prefetch guard.
     * Both spans must be disjoint. Address fields encode bits [33:2], but
     * admission must establish the actual bus/IOMMU addressability separately.
     */
    uint64_t mapped_bytes;
};
struct a733_camera_dma_config {
    struct a733_camera_plane y, uv;
    uint64_t frame_id; /* Nonzero caller identity, immutable through close. */
    uint8_t block; /* Physical DMA 0..5; channel 0, entire block exclusive. */
};
struct a733_camera_dma_ops {
    int (*now_us)(void *, uint64_t *);
    /* Return 1 complete, 0 pending, -1 failure. Publish a nonzero token before
     * any side effect and preserve it thereafter. Admission owns both spans,
     * clocks, reset, full physical block, route and one-frame producer fence.
     * Prove CSI master-1 mapping, guard spans, cache isolation and independent
     * recovery. Prepare cache for device writes. Producer remains quiet.
     * VIPP must deliver progressive YUV422 of the configured size without
     * scale/crop/OSD.
     * Native route/VIPP/gating implementations are not supplied by this API.
     */
    int (*admit)(void *, const struct a733_camera_dma_config *, uint64_t,
                 uint64_t *token);
    /* Aligned ordered Device accesses, base 0x05830000 + block*0x1000.
     * MMIO callbacks return 1 or -1; every successful write includes barriers.
     */
    int (*read)(void *, uint64_t token, uint16_t off, uint32_t *, uint64_t);
    int (*write)(void *, uint64_t token, uint16_t off, uint32_t, uint64_t);
    /* Release exactly one complete frame into the admitted path. The fence
     * must prevent even a partial second frame from reaching the DMA.
     * Use parser SCAP image mode; completion requires DMA CAPTURE_DONE.
     */
    int (*start_one)(void *, uint64_t token, uint64_t);
    /* Establish quiet input and successful single-frame fence operation.
     * For explicit retained readout, establish the completed SCAP fence and
     * captured-frame error checks instead. Global ingress quiet is not claimed;
     * all local/global resources and mappings remain owned without release. */
    int (*producer_stop)(void *, uint64_t token, uint64_t);
    /* After CAP_EN clear, prove all writes retired, no prefetched addresses,
     * no upstream errors, and exactly one complete undamaged frame. Register
     * enable readback and CAPTURE_DONE alone do not establish these properties.
     */
    int (*drain)(void *, uint64_t token, uint64_t);
    /* Cache invalidation/acquire barriers only. Ownership stays with driver
     * until poll returns READY; a failure retains both spans and the token.
     */
    int (*sync_cpu)(void *, uint64_t token, uint64_t);
    /* Release mapping/resource leases after READY. Caller keeps frame data. */
    int (*release)(void *, uint64_t token, uint64_t);
    /* Optional, for continuous capture only. start_stream establishes that
     * the producer runs continuously into the admitted path. sync_slot
     * invalidates the CPU cache over both planes of one complete slot, which
     * the device no longer addresses, with acquire barriers. Both return
     * 1, 0 pending or -1, with the retention rules of the operations above. */
    int (*start_stream)(void *, uint64_t token, uint64_t);
    int (*sync_slot)(void *, uint64_t token, unsigned slot, uint64_t);
};
struct a733_camera_dma {
    struct a733_camera_dma_ops ops;
    struct a733_camera_dma_config config;
    void *context;
    uint64_t token, now, deadline;
    uint32_t saved, events;
    struct a733_camera_format format;
    struct a733_camera_ring ring;
    enum a733_camera_dma_state state;
    enum a733_camera_frame_owner owner;
    uint16_t index;
    uint8_t phase, initialized, retained_readout, stream;
};
int a733_camera_dma_init(struct a733_camera_dma *,
    const struct a733_camera_dma_ops *, void *, const struct a733_camera_dma_config *);
/* Same, for any valid format; each plane span must cover its plane bytes. */
int a733_camera_dma_init_format(struct a733_camera_dma *,
    const struct a733_camera_dma_ops *, void *, const struct a733_camera_dma_config *,
    const struct a733_camera_format *);
int a733_camera_dma_open(struct a733_camera_dma *, uint64_t deadline_us);
int a733_camera_dma_capture(struct a733_camera_dma *, uint64_t deadline_us);
int a733_camera_dma_close(struct a733_camera_dma *, uint64_t deadline_us);
/* Opt-in access without ownership transfer, only from STOP after this object's
 * actual CAPTURE_DONE observation. The coordinating producer_stop operation
 * must authenticate self-cleared SCAP, no rearm, source/error observations and
 * the same retained mappings. Fresh DMA status and CPU synchronization still
 * execute. No disable, drain, status acknowledgement or release occurs.
 * READABLE keeps DEVICE ownership; only the width x height payload of each
 * plane may be read. Neither padding nor guards are image data. No close/release or
 * mapping change is authorized; only rearm starts another image in the planes. */
int a733_camera_dma_readout(struct a733_camera_dma *,uint64_t deadline_us);
/* Return a READABLE channel to ARMED for one more image into the same planes
 * and mappings. The caller declares every view and copy of the previous image
 * ended and keeps the producer stopped behind a cleared SCAP fence. Status
 * must still equal the final observation, which alone is acknowledged; status
 * must then read clear, capture inactive and the channel still enabled. The
 * admit callback runs again with the retained token to prepare the cache for
 * device writes. Ownership stays DEVICE; faults retain everything. */
int a733_camera_dma_rearm(struct a733_camera_dma *,uint64_t deadline_us);
/* Continuous capture from ARMED, with the parser in video capture mode and
 * the producer stopped. slots (2..A733_CAMERA_DMA_SLOTS) frame slots of
 * slot_bytes each (a multiple of 4096 that covers one plane) must fit both
 * spans; every slot stays device-owned until delivered. Slot 0 is already
 * programmed, start_stream runs, and STREAMING then reads status once per
 * poll. The address registers latch at each frame start, as the reference
 * driver's frame-start handler assumes. Each observed frame start makes the
 * programmed slot active, delivers the previous active slot if a frame-done
 * observation followed its start, and programs the next free slot. With no
 * free slot the active slot repeats and its frame is dropped. After each
 * address write a status read checks for a new frame start; if one
 * arrived, the write may have missed it, so the frame is counted as dropped
 * and no slot is delivered until the following start. A slot is never
 * delivered while an address register can name it. Delivered slots are
 * invalidated by sync_slot, then queued in capture order. Errors fault. */
int a733_camera_dma_stream(struct a733_camera_dma *, unsigned slots,
                           uint64_t slot_bytes, uint64_t deadline_us);
/* Oldest delivered slot and its frame-start count; 1, 0 if none, -1 if not
 * streaming. The slot's two planes become readable by the caller only, and
 * no MMIO occurs. give returns a HELD slot to the device. */
int a733_camera_dma_take(struct a733_camera_dma *, unsigned *slot, uint32_t *sequence);
int a733_camera_dma_give(struct a733_camera_dma *, unsigned slot);
/* From STREAMING with no slot held: enter STOP for the disable, drain and
 * final status sequence. The producer must stop and be fenced before the
 * next poll. Final status needs no CAPTURE_DONE, and READY then returns
 * both complete spans to the caller. */
int a733_camera_dma_stop_stream(struct a733_camera_dma *, uint64_t deadline_us);
/* At most one MMIO/platform operation plus pre/post monotonic time checks.
 * Faults are terminal and retain ownership; no automatic stop/reset/retry.
 * READY alone returns frame ownership. ARMED has not captured any data.
 */
int a733_camera_dma_poll(struct a733_camera_dma *);
#endif
