/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_CAMERA_NATIVE_H
#define SUNSTONE_A733_CAMERA_NATIVE_H
#include "a733_camera_capture.h"
#include "a733_twi.h"
#include "a733_media_clock.h"
#include "a733_pmic.h"
#include "device_memory_pool.h"

#define A733_CAMERA_NATIVE_PIN_COUNT 15u
#define A733_CAMERA_NATIVE_PLANE_BYTES 307200u
#define A733_CAMERA_NATIVE_GUARD_BYTES 4096u
#define A733_CAMERA_NATIVE_BYTES 663552u /* Tables 8 + parameters 2 + two 76-page planes. */
/* Fixed device address of the parameters. The planes follow at +8192 and
 * +319488, all three inside one 4 MiB IOMMU window below 4 GiB. The IOMMU
 * translates them to the allocation, which may lie anywhere below 2^34. */
#define A733_CAMERA_NATIVE_IOVA UINT64_C(0x10000000)
#define A733_CAMERA_NATIVE_FILL 0xa5u /* Plane payload/guard fill before each device write. */
#define A733_CAMERA_NATIVE_DISCARD_LIMIT 15u
/* Protected image/launch record, authenticated by the actual boot coordinator.
 * A nonzero identity, matching digest or these fields alone is not evidence.
 * Host policy must bind the complete record to the linked module profile,
 * exact build/epoch/lease, qualified reset/analog/electrical/recovery evidence,
 * and resolved prior transactions/walks for all ten IOMMU masters. The physical
 * owner excludes remote firmware and every local master until this image ends.
 * Both IOMMU banks and all ten master's power domains remain accessible.
 * Current memory-only boot admission does NOT produce this authority.
 * test_pattern 0 captures the ordinary sensor image; 1 selects the OV5640
 * color bars (register 0x503d = 0x84). pwdn_only 1 admits a module whose
 * RESETB has no separate GPIO: PE6 power-down is its only control, reset_bank,
 * reset_pin and inherited[14] are zero, and only fourteen pins are claimed.
 * discard_frames (at most A733_CAMERA_NATIVE_DISCARD_LIMIT) images are captured
 * and rearmed over, unread, before the image that becomes readable; zero keeps
 * the single exposure. Each is a complete stop/start cycle of the sensor.
 */
struct a733_camera_boot_record {
    uint64_t version,bytes,owner,epoch,generation,lease,deadline_us;
    uint64_t build[4],evidence[4];
    uint32_t proof,origin,excluded_masters,powered_masters;
    uint32_t raw_trim,parameter_guard_bytes,plane_guard_bytes;
    uint32_t apb1,apb1_hz;
    uint8_t settle,deskew,reset_bank,reset_pin,test_pattern,pwdn_only;
    struct a733_pin_config inherited[A733_CAMERA_NATIVE_PIN_COUNT];
    uint8_t discard_frames; /* Fills the alignment gap before rails. */
    enum a733_rail rails[3];
    uint32_t programmed_uv[3];
};
struct a733_camera_native_config {
    const struct a733_camera_boot_record *boot; /* Protected retained record. */
    struct a733_media_clock *media;
    struct a733_clock_leases *clocks;
    struct a733_pins *pins;
    struct a733_rail_leases *rails;
    const struct a733_rail_token *rail_parent;
    struct a733_pmic *pmic;
    struct a733_platform_locks *locks;
    void *task;
};
enum a733_camera_native_state {
    A733_CAMERA_NATIVE_NEW,A733_CAMERA_NATIVE_CLOCK,A733_CAMERA_NATIVE_RAIL,
    A733_CAMERA_NATIVE_SNAPSHOT,A733_CAMERA_NATIVE_PINS,A733_CAMERA_NATIVE_TWI,
    A733_CAMERA_NATIVE_MCLK,A733_CAMERA_NATIVE_CAPTURE,A733_CAMERA_NATIVE_READABLE,
    A733_CAMERA_NATIVE_FAULT
};
struct a733_camera_native {
    struct a733_camera_native *self;
    struct a733_camera_native_config config;
    struct a733_camera_boot_record boot;
    struct a733_clock_token twi_clock,media_clock;
    struct a733_rail_borrow rail;
    struct a733_pmic_request request;
    struct a733_pin_lease_banked pin;
    struct a733_twi_native twi_native;
    struct a733_twi twi;
    struct a733_camera_capture capture;
    struct device_memory_pool *pool;
    struct device_memory_lease memory;
    struct device_memory_extent extent;
    uint64_t last_now;
    unsigned snapshot,index,held,entered,started,discarded;
    enum a733_camera_native_state state;
};
/* Actual singleton allocation during serialized primary preparation, before
 * any concurrent allocator user. One retained parent contains tables, immutable
 * parameters, both planes and guards. No new pool, alternate alias or fallback
 * range. Tables align 16 KiB. The spans use A733_CAMERA_NATIVE_IOVA and its
 * fixed offsets. The allocation's physical range must end at or below 2^34;
 * its physical address need not equal its CPU address. Both planes and their guards are filled with
 * A733_CAMERA_NATIVE_FILL while CPU-owned, so bytes the DMA never writes cannot
 * hold an earlier image. Failed construction retains acquired backing.
 * This does not qualify the 4 KiB guard policy; the boot evidence must do so.
 * Parent/media/pin/rail/provider objects remain private and platform-lived. */
int a733_camera_native_init(struct a733_camera_native *,const struct a733_camera_native_config *);
/* Version 2 boot record and exact SUPERVISED_PROOF; same retained providers and
 * controllers. Manual whole-board power removal replaces watchdog admission,
 * never actual reset/drain/exclusion, mapped guards or live ownership. */
int a733_camera_native_supervised_init(struct a733_camera_native *,const struct a733_camera_native_config *);
/* Designated EL1 task advances actual media-token delegation, rail borrows and
 * queued PMIC observations, checked pins, TWI, MCLK and the full capture path.
 * PMIC's existing provider task must run independently. Missing/faulted/late
 * prerequisites refuse before the dependent operation. No rollback/retry;
 * only a registry status read that reports BUSY is repeated, until deadline.
 * Returns 0 pending, 1 retained read-only frame ready, -1 retained fault. */
int a733_camera_native_poll(struct a733_camera_native *);
/* Serialized privileged consumer. Copies only admitted image payload into a
 * distinct actual CPU-owned lease from the same native singleton, registered
 * to this owner. The application's allocation remains pinned and cannot be
 * released, published to another device or mutated concurrently with the call.
 * No arbitrary pointer, metadata target or guard byte is accepted. Failure may leave a
 * partial destination, which is untrusted; source backing remains reserved.
 * The caller must not expose the destination until 1 is returned. No EL0
 * address is accepted merely because it translates; an EL0 service needs its
 * own client admission. */
int a733_camera_native_copy(struct a733_camera_native *,unsigned plane,
    unsigned offset,struct device_memory_lease destination,size_t destination_offset,size_t bytes);
#endif
