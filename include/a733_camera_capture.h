/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_CAMERA_CAPTURE_H
#define SUNSTONE_A733_CAMERA_CAPTURE_H
#include "ov5640.h"
#include "a733_camera_platform.h"
#include "a733_csi_receiver.h"
#include "a733_camera_route.h"
#include "a733_camera_dma.h"
#include "a733_iommu.h"

enum a733_capture_memory { A733_CAPTURE_PARAMETERS,A733_CAPTURE_Y,A733_CAPTURE_UV };
struct a733_capture_span {
    void *cpu;
    uint64_t iova,physical,bytes; /* Complete exclusive page spans, including guards. */
};
enum a733_capture_handoff { A733_CAPTURE_LOCAL,A733_CAPTURE_PACKETS };
struct a733_camera_capture;
struct a733_camera_capture_config;
struct a733_capture_ops {
    int (*now_us)(void *,uint64_t *);
    /* Global owner: reserve both IOMMU banks, all ten masters, table arena,
     * parameter/frame pages, common analog and all local camera resources.
     * Publish stable nonzero token before acquisition; return 1/0/-1.
     * Establish actual CPU/physical aliases, permissions, full prefetch guards,
     * cache isolation, clocks/pins/power/reset handoff and independent recovery.
     * Every leaf admission below must authenticate a sublease of this owner.
     * Quiet sensor and all masters; acquisition must not enable any producer.
     */
    int (*acquire)(void *,const struct a733_camera_capture_config *,uint64_t,uint64_t *);
    /* Authenticate the retained global/analog/IOMMU tokens and assembled path.
     * LOCAL permits only this owner's selected receiver to configure its quiet
     * local PHY/parser. PACKETS permits only this sensor to emit into armed
     * SCAP/route/DMA0. No register mutation or producer enable in this callback.
     * Common words stay exclusive; all other nine IOMMU masters stay excluded.
     * Both banks' clocks and all ten master power domains remain accessible
     * through the final fault scan. Published tables and span metadata stay
     * immutable and exclusively owned after capture completion.
     * Return 1/0/-1; authority remains valid through completion or retained fault.
     */
    int (*handoff)(void *,uint64_t,enum a733_capture_handoff,
                   const struct a733_camera_capture *,uint64_t);
    /* After native sensor stop/readback, prove no further packets or in-flight
     * ingress can change receiver status. A sensor register readback alone is
     * insufficient. Return 1/0/-1; no resets or resource release.
     */
    int (*packets_quiet)(void *,uint64_t,uint64_t);
};
struct a733_camera_capture_config {
    struct a733_capture_ops ops;
    void *context; /* Shared by the supplied platform callbacks. */
    struct a733_capture_span memory[3]; /* READ parameters; WRITE Y and UV. */
    uint32_t *table_words; /* Exclusive 32KiB CPU/physical arena, 16KiB aligned. */
    uint64_t table_physical,frame_id;
    struct a733_csi_config receiver; /* Matching port/parser 0 or 1. */
    int color_bars;
    /* Existing native components execute through these platform operations.
     * now_us members are ignored in favor of the shared clock above.
     * route/receiver producer_stop and DMA start_one/producer_stop are ignored:
     * the coordinator supplies them from actual native component state.
     * DMA drain is called once after native path disable and before the final
     * IOMMU scan; it must independently prove complete frame/write/walk drain.
     * All release callbacks relinquish LOCAL subleases only, preserving global
     * exclusion, common analog power/clocks, IOMMU mappings and table ownership.
     * No platform callback may synthesize native controller success, enable a
     * producer, retry, reset, change tables or perform an unrequested rollback.
     */
    struct ov5640_ops sensor;
    struct a733_camera_platform_ops analog;
    struct a733_iommu_ops iommu;
    struct a733_route_ops route;
    struct a733_csi_ops csi;
    struct a733_camera_dma_ops dma;
};
enum a733_capture_state {
    A733_CAPTURE_NEW,A733_CAPTURE_ACQUIRE,A733_CAPTURE_ANALOG,
    A733_CAPTURE_IOMMU,A733_CAPTURE_SENSOR_OPEN,A733_CAPTURE_ROUTE_OPEN,
    A733_CAPTURE_DMA_OPEN,A733_CAPTURE_LOCAL_HANDOFF,A733_CAPTURE_RECEIVER_OPEN,
    A733_CAPTURE_PACKET_HANDOFF,A733_CAPTURE_SENSOR_START,A733_CAPTURE_DMA_RUN,
    A733_CAPTURE_SENSOR_STOP,A733_CAPTURE_QUIET,A733_CAPTURE_SAMPLE,
    A733_CAPTURE_FENCE,A733_CAPTURE_DMA_DISABLE,A733_CAPTURE_ROUTE_CLOSE,
    A733_CAPTURE_RECEIVER_CLOSE,A733_CAPTURE_SENSOR_CLOSE,A733_CAPTURE_DRAIN,
    A733_CAPTURE_IOMMU_SCAN,A733_CAPTURE_DMA_FINISH,A733_CAPTURE_DMA_CLOSE,
    A733_CAPTURE_READY,A733_CAPTURE_FAULT,A733_CAPTURE_READABLE,A733_CAPTURE_READOUT_ROUTE
};
struct a733_camera_capture {
    struct a733_camera_capture_config config;
    struct a733_iommu_tables tables;
    struct ov5640 sensor;
    struct a733_camera_platform analog;
    struct a733_iommu iommu;
    struct a733_camera_route route;
    struct a733_csi_receiver receiver;
    struct a733_camera_dma dma;
    uint64_t token,deadline,now;
    enum a733_capture_state state;
    uint8_t initialized,entered,started,local_granted,packets_granted;
    uint8_t quiet,sampled,fenced,drained,scanned,retained_readout;
};
/* Fresh zero storage; private immutable config/callback storage; one privileged
 * serialized caller; bounded non-reentrant callbacks. CPU-only table construction
 * occurs here; all aliases must
 * already be exclusively reserved for this owner. Failed initialization consumes
 * the object but does not touch hardware. No allocations or hidden retries.
 */
int a733_camera_capture_init(struct a733_camera_capture *,const struct a733_camera_capture_config *);
/* Same controller path, explicit manual-power-removal retained lifetime.
 * All non-recovery physical admission requirements remain unchanged. */
int a733_camera_capture_supervised_init(struct a733_camera_capture *,
    const struct a733_camera_capture_config *);
int a733_camera_capture_open(struct a733_camera_capture *,uint64_t deadline);
/* Source-defined still-frame access while every resource stays reserved.
 * After actual CAPTURE_DONE, stop the sensor, sample CSI errors, verify all
 * parser capture bits clear, scan IOMMU faults, and perform fresh DMA status
 * and CPU cache synchronization. No packet-quiet/drain/close/release callback
 * runs. READABLE preserves DEVICE ownership and all mappings/table backing.
 * Only the 307,200-byte payload per plane may be read, without mutation or reuse.
 * A privileged const view cannot be revoked: its consumer must serialize use
 * with this owner and stop on a later fault. Only a completed separate copy
 * remains independently usable. This API does not authorize EL0 aliases. */
int a733_camera_capture_open_retained(struct a733_camera_capture *,uint64_t deadline);
/* One platform/MMIO/cache operation at most, plus nested monotonic clock reads.
 * READY transfers the 640x480 NV16 Y/UV payloads to this caller, stride640 each.
 * Global/analog/IOMMU tokens and mappings remain retained even after READY.
 * No restart, dispose, cancellation or automatic rollback. FAULT retains every
 * unreleased sublease and the global reservation; it performs no further I/O.
 * Physical clock/pin/power binding and qualification remain prerequisites.
 */
int a733_camera_capture_poll(struct a733_camera_capture *);
#endif
