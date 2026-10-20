/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_DP_H
#define SUNSTONE_A733_DP_H
#include <stdint.h>
#include <stddef.h>
#define A733_DP_BASE UINT32_C(0x05740000)
#define A733_DP_PAD_BASE UINT32_C(0x05760000)
#define A733_DP_TV_BASE UINT32_C(0x05731000)
#define A733_DP_TOP_BASE UINT32_C(0x05510000)
/* Closed profile: four DP-only lanes, 1.62Gb/s, RGB888 720p60, one immutable
 * DE1 RGB565 surface. Source-dependent physical admission belongs to native
 * construction. Callback success is exactly1; failure permanently retains. */
enum a733_dp_error { A733_DP_OK,A733_DP_ARGUMENT,A733_DP_AUTHORITY,
    A733_DP_TIME,A733_DP_IO,A733_DP_PHY,A733_DP_AUX,A733_DP_SINK,
    A733_DP_EDID,A733_DP_TRAINING,A733_DP_SOURCE,A733_DP_STATUS };
enum a733_dp_state { A733_DP_NEW,A733_DP_PREPARING,A733_DP_READY,
    A733_DP_ACTIVE,A733_DP_STOPPED_RETAINED,A733_DP_FAILED_RETAINED };
struct a733_dp_result {
    uint32_t state,error,stage,retained;
    uint32_t aux_requests,aux_defers,cr_attempts,eq_attempts;
    uint32_t aux_status,aux_reply,aux_count,phy_status;
    uint32_t frame_status,fifo_status,tv_status,edid_bytes;
    uint8_t dpcd[16],link_status[6],training[4],reserved[6],edid[256];
};
_Static_assert(sizeof(struct a733_dp_result)==352,"DP copied controller result");
struct a733_dp_ops {
    int (*admit)(void *,unsigned); /* 0 prepare/service,1 immutable DE ARMED,2 output stopped. */
    int (*read)(void *,uintptr_t,unsigned,uint32_t *);
    int (*write)(void *,uintptr_t,unsigned,uint32_t);
    int (*barrier)(void *);
    int (*now)(void *,uint64_t *);
    int (*wait)(void *,uint32_t,uint64_t); /* actual elapsed minimum, yields task, no held mutex */
};
struct a733_dp_config {
    struct a733_dp_ops ops;
    void *context;
    uint64_t owner,deadline;
    uint32_t polarity,apb_hz;
};
struct a733_dp {
    struct a733_dp *self;
    struct a733_dp_config config,sealed;
    struct a733_dp_result result;
    uint64_t last_now;
    unsigned entered,failed,phy_ready,stream_started;
};
/* Init has no device access. One designated privileged task owns all methods.
 * Sources, permanent exclusions and qualified supplies/clocks precede prepare.
 * Each operation has a finite request/poll limit and the immutable absolute
 * deadline; callbacks must not block beyond that deadline. No release/retry. */
int a733_dp_init(struct a733_dp *,const struct a733_dp_config *);
int a733_dp_prepare(struct a733_dp *);
int a733_dp_start(struct a733_dp *);
int a733_dp_service(struct a733_dp *);
int a733_dp_stop(struct a733_dp *);
int a733_dp_copy_result(struct a733_dp *,struct a733_dp_result *);
#endif
