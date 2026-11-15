/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_PLATFORM_H
#define SUNSTONE_A733_VIDEO_PLATFORM_H
#include "a733_video_jpeg_run.h"
#include "a733_video_h264.h"
#include "a733_video_hevc.h"
#include "a733_video_unit.h"
#include "a733_power_domain_row.h"

/* Privileged immutable configuration. Borrow exact live tokens from the ONE
 * clock registry and PCK resource manager; neither may be released until a
 * separate recovery/drain owner proves all codec effects finished. Fabric is
 * adopted at observed rates. Parent clocks are too, except that a cold start
 * at the vendor operating point (module selector and divider
 * A733_VIDEO_VENDOR_MODULE, module_hz A733_VIDEO_VENDOR_HZ) programs PLL_VE0.
 * module contains source/divider/gate bits only. First binding accepts VE0/VE1
 * parents. fabric[] contains AHB, NSI and MBUS source/divider/gate fields.
 * The authority callback must validate the board rail and operating point at
 * module_hz: a caller-supplied number does not establish electrical validity.
 */
enum a733_video_start { A733_VIDEO_INHERITED = 0, A733_VIDEO_COLD_NS_ROM = 1 };
/* A cold start (COLD_NS_ROM) needs row: the consumer's registered domain-2
 * row (a733_power_domain_row_init in a733_power_domains.h), whose rail token
 * holds DCDC2. The binding requests ON once after its read-only entry and
 * returns pending until the owner answers; the caller blocks on the row's
 * wake and calls again. The binding writes no PCK600 register. */
struct a733_video_platform_config {
    struct a733_clock_leases *clocks;
    struct a733_clock_token clock;
    struct a733_gpu_resource_manager *power;
    struct a733_gpu_resource_lease pck;
    unsigned power_client;
    uint64_t owner, handoff;
    uint32_t module, module_hz, fabric[3], fabric_hz[3];
    enum a733_video_start start;
    struct a733_power_domain_row *row;
};
struct a733_video_platform_ops {
    int (*now_us)(void *,uint64_t *);
    /* Authenticate startup authority, reserve both masters and IRQ81, freeze
     * job input and aliases, and establish quiet/drained inherited state plus
     * independent recovery for legacy drain, or explicit supervised permanent
     * retention until whole-board power removal for retained-copy use. Neither
     * policy substitutes for initial transaction/walk drain. Require PCK domain2 ON and an identity-readable
     * reset-deasserted decoder in INHERITED mode. COLD_NS_ROM instead requires
     * genuine, fixed, readable NS-ROM mapping, decoder OFF/reset held, and
     * exclusive cold-start authority. ROM bytes are observed by this binding;
     * caller claims alone never identify silicon. This callback must not change hardware or
     * translations. Nonzero stable token BEFORE any admission; retain always.
     * Also validate rail/operating point, Device mappings and common mutex.
     * Return1 complete,0 pending,-1 uncertain. The fixed JPEG application supplies the supervised retained authority.
     */
    int (*admit)(void *,const struct a733_video_platform_config *,
                 const struct a733_video_jpeg_job *,uint64_t,uint64_t *);
    /* After supported identity: map both masters, exclude physical aliases,
     * establish full input/output burst extents, publish input/output caches.
     * No VE/clock/reset writes; retain every reservation. Return1/0/-1.
     * Successful return authorizes reset only because admit proved prior drain.
     */
    int (*memory)(void *,uint64_t,const struct a733_video_jpeg_job *,
                  const struct a733_video_jpeg *,uint32_t,uint32_t,uint64_t);
    /* Completion independently establishes producer stop, accepted-transaction
     * drain, fault-free masters and output-cache invalidation. Return1/0/-1.
     * No releases; no interpretation of VE status as external drain.
     */
    int (*drain)(void *,uint64_t,uint64_t);
    /* ONE platform register mutex. enter1 acquired,0 effect-free busy,-1
     * uncertain. leave exactly1. On uncertain access this controller retains
     * the held mutex and every token. Serialized privileged caller only.
     */
    int (*enter)(void *,uint64_t);
    int (*leave)(void *);
    /* Exact physical-address allowlist; width1/4. Return exactly1 after access
     * and completion barrier. Model callbacks may substitute storage. Native
     * callbacks below require identity-mapped Device-nGnRnE pages at EL1.
     */
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,unsigned,uint32_t);
    /* Optional plain per-core clock for the PLL lock-wait gaps (struct a733_video_unit_ops). */
    int (*spin_now_us)(void *,uint64_t *);
};
/* Codec-neutral decoder sequencing on the shared VE resource core: admission,
 * inherited or cold start, identity, memory and reset/default preparation.
 * Each codec binding passes its own typed job through admit and memory; the
 * job pointer is the binding's object, never another codec's structure. */
/* What a cold start saw, for the caller's record: the three critical gates
 * at entry and at A1, the 0x5e4 word written and read back when bit 18 was
 * opened (zero when no write was needed), the times (binding clock, us) at
 * the start and end of the entry hold, the row's ON answer, the end of hold
 * A, the end of the PLL lock wait and the end of hold B, and the lock wait's
 * PLL reads. */
struct a733_video_cold_record {
    uint32_t entry_gates[3], gates[3], mclk_written, mclk_readback;
    uint64_t entry_at, entered_at, on_at, hold_a_at, locked_at, hold_b_at;
    unsigned lock_reads;
};
struct a733_video_decoder {
    struct a733_video_unit unit;
    struct a733_video_platform_config config;
    int (*admit)(void *,const struct a733_video_platform_config *,const void *,uint64_t,uint64_t *);
    int (*memory)(void *,uint64_t,const void *,uint64_t);
    void *context;
    uint64_t version,reset_until;
    unsigned admitted,checked,mapped,prepared,step,cold_step,requested,quiesced;
    struct a733_video_cold_record cold;
};
/* JPEG binding. */
struct a733_video_platform {
    struct a733_video_decoder decoder;
    struct a733_video_platform_ops ops;
    void *cookie;
};
/* Quiesce after a cold start, under one register-lock hold, in the vendor's
 * order (cedar_ve.c disable_cedar_hw_clk_module and disable_cedar_hw_clk_reset,
 * ccu_gate.c ccu_pll_gate_helper_disable): module gate off, bus gate off,
 * reset asserted, then PLL_VE0's output gate, lock enable, enable and LDO
 * cleared. Every write is a read-modify-write with readback. Domain 2 stays
 * ON and the critical gates stay as they are. The caller ensures no job is
 * outstanding. d is a new deadline for this call. 1 done, 0 effect-free busy
 * lock, -1 refused or uncertain (a failed unit is never touched). A quiesced
 * decoder is closed for the rest of its power lifetime: every later acquire
 * returns -1 with no register access. */
int a733_video_decoder_quiesce(struct a733_video_decoder *,uint64_t d);
/* Pure scope derivation; requires the exclusive scopes below and the exact
 * shared result.
 * Preparation never releases, powers down, or claims physical qualification.
 */
int a733_video_platform_scopes(const struct a733_video_platform_config *,a733_clock_mask_t *);
/* The exclusive scopes to acquire with the shared ones: VE_DEC, and PLL_VE0
 * for a cold start at the vendor operating point, which programs it. 0 for
 * a refused configuration. */
a733_clock_mask_t a733_video_platform_exclusive(const struct a733_video_platform_config *);
int a733_video_platform_init(struct a733_video_platform *,
    const struct a733_video_platform_config *,const struct a733_video_platform_ops *,void *);
/* Immutable adapter table for a733_video_jpeg_begin(...,p,...). */
const struct a733_video_jpeg_ops *a733_video_platform_jpeg_ops(void);
/* Distinct retained-copy gate. Validates actual prepared platform/token/time
 * and common leases before calling the bounded private-copy verifier. Its1
 * permits only the verified CPU copy; no DMA drain, ownership transfer, release
 * or buffer reuse. All reservations persist until whole-board power removal.
 * Initial isolation/drain and source-derived mappings remain prerequisites. */
int a733_video_platform_retained_copy(void *,uint64_t,uint64_t,
    int (*)(void *,uint64_t,uint64_t),void *);
/* Read/write only within the checked platform/codec allowlists, through
 * identity-mapped Device-nGnRnE pages. Caller holds admitted ownership and
 * mutex; these functions do not acquire authority.
 * Non-AArch64 builds refuse; no host model can silently become native I/O.
 */
int a733_video_platform_native_read(void *,uintptr_t,uint32_t *);
int a733_video_platform_native_write(void *,uintptr_t,unsigned,uint32_t);
/* The native write allowlist as a pure rule, for native_write and models:
 * returns 1 allowed with the owned mask for a733_video_unit_native_write, 0
 * refused and -1 when read fails. read observes the PLL_VE0 consumers. */
int a733_video_platform_write_policy(uintptr_t,unsigned,uint32_t,int (*read)(void *,uintptr_t,uint32_t *),
    void *,uint32_t *owned);

/* One fixed H.264 consumer of the same decoder sequencing and leases. The
 * H.264 job stays typed and is never presented as a JPEG picture or scan.
 * Callback contracts are the same retained acquisition/memory contracts above;
 * memory supplies the six explicit H.264 spans, including scratch and MV data.
 */
struct a733_h264_platform_ops {
    int (*now_us)(void *,uint64_t *);
    int (*admit)(void *,const struct a733_video_platform_config *,const struct a733_h264_job *,uint64_t,uint64_t *);
    int (*memory)(void *,uint64_t,const struct a733_h264_job *,uint64_t);
    int (*copied)(void *,uint64_t,uint64_t);
    int (*enter)(void *,uint64_t);
    int (*leave)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,unsigned,uint32_t);
};
struct a733_h264_platform {
    struct a733_video_decoder decoder;
    struct a733_h264_platform_ops ops;
    struct a733_h264_job job;
    void *cookie;
    unsigned bound;
};
int a733_h264_platform_init(struct a733_h264_platform *,const struct a733_video_platform_config *,
                            const struct a733_h264_platform_ops *,void *);
const struct a733_h264_ops *a733_video_platform_h264_ops(void);

/* One fixed HEVC consumer of the same decoder sequencing and leases, with its
 * own typed job and H.265 register allowlist. memory supplies the eight
 * explicit HEVC spans. copied receives the picture whose output it verifies;
 * the retained-copy contract above applies unchanged.
 */
struct a733_hevc_platform_ops {
    int (*now_us)(void *,uint64_t *);
    int (*admit)(void *,const struct a733_video_platform_config *,const struct a733_hevc_job *,uint64_t,uint64_t *);
    int (*memory)(void *,uint64_t,const struct a733_hevc_job *,uint64_t);
    int (*copied)(void *,uint64_t,unsigned,uint64_t);
    int (*enter)(void *,uint64_t);
    int (*leave)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,unsigned,uint32_t);
};
struct a733_hevc_platform {
    struct a733_video_decoder decoder;
    struct a733_hevc_platform_ops ops;
    struct a733_hevc_job job;
    void *cookie;
    unsigned bound,copy_picture;
};
int a733_hevc_platform_init(struct a733_hevc_platform *,const struct a733_video_platform_config *,
                            const struct a733_hevc_platform_ops *,void *);
const struct a733_hevc_ops *a733_video_platform_hevc_ops(void);
#endif
