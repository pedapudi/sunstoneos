/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_VIDEO_PLATFORM_H
#define SUNSTONE_A733_VIDEO_PLATFORM_H
#include "a733_video_jpeg_run.h"
#include "a733_video_h264.h"
#include "a733_clock_leases.h"
#include "a733_gpu_resources.h"

/* Privileged immutable configuration. Borrow exact live tokens from the ONE
 * clock registry and PCK resource manager; neither may be released until a
 * separate recovery/drain owner proves all codec effects finished. Parent
 * clocks and fabric are adopted at observed rates, never retuned here.
 * module contains source/divider/gate bits only. First binding accepts VE0/VE1
 * parents. fabric[] contains AHB, NSI and MBUS source/divider/gate fields.
 * The authority callback must validate the board rail and operating point at
 * module_hz: a caller-supplied number does not establish electrical validity.
 */
enum a733_video_start { A733_VIDEO_INHERITED = 0, A733_VIDEO_COLD_NS_ROM = 1 };
struct a733_video_platform_config {
    struct a733_clock_leases *clocks;
    struct a733_clock_token clock;
    struct a733_gpu_resource_manager *power;
    struct a733_gpu_resource_lease pck;
    unsigned power_client;
    uint64_t owner, handoff;
    uint32_t module, module_hz, fabric[3], fabric_hz[3];
    enum a733_video_start start;
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
};
struct a733_video_platform {
    struct a733_video_platform_config config;
    struct a733_video_platform_ops ops;
    void *cookie;
    uint64_t token,deadline,last_now,version,reset_until;
    a733_clock_mask_t shared;
    uint32_t selected_mhz[3],identity_high;
    unsigned initialized,admitted,checked,mapped,prepared,step,failed,locked,cold_step;
};
/* Pure scope derivation; requires exclusive VE_DEC and exact shared result.
 * Preparation never releases, powers down, or claims physical qualification.
 */
int a733_video_platform_scopes(const struct a733_video_platform_config *,a733_clock_mask_t *);
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
/* Read/write only within the checked platform/codec allowlists. Caller holds
 * admitted ownership and mutex; these functions do not acquire authority.
 * Non-AArch64 builds refuse; no host model can silently become native I/O.
 */
int a733_video_platform_native_read(void *,uintptr_t,uint32_t *);
int a733_video_platform_native_write(void *,uintptr_t,unsigned,uint32_t);

/* One fixed H.264 consumer, sharing the actual cold/clock/reset/lease provider.
 * The JPEG structure and callback ABI above are unchanged. This wrapper keeps
 * the H.264 job typed and never presents it as a JPEG picture or scan.
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
    struct a733_video_platform common;
    struct a733_h264_platform_ops ops;
    struct a733_h264_job job;
    void *cookie;
    unsigned bound;
};
int a733_h264_platform_init(struct a733_h264_platform *,const struct a733_video_platform_config *,
                            const struct a733_h264_platform_ops *,void *);
const struct a733_h264_ops *a733_video_platform_h264_ops(void);
#endif
