/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_PMIC_CONSUMER_H
#define SUNSTONE_A733_PMIC_CONSUMER_H
#include "a733_pmic.h"
struct a733_pmic_consumer_config {
    struct a733_pmic *provider;
    struct a733_pmic_lifetime lifetime;
    uint64_t owner;
    enum a733_rail rail;
};
/* Private, immovable, single-task storage for a platform-lifetime
 * accelerator supply: DCDC2 at 800 mV, or DCDC4 at the GPU operating point's
 * 960 mV (a733_gpu_opp.h), which the GPU platform sets before its first
 * sample. The completed request retains its real provider borrow.
 * No consumer release or automatic retry is provided after uncertainty. */
struct a733_pmic_consumer {
    struct a733_pmic_consumer *self;
    struct a733_pmic_consumer_config config, sealed;
    struct a733_rail_token rail;
    struct a733_pmic_request request;
    struct a733_pmic_snapshot snapshot;
    uint64_t deadline, last_now, armed_deadline;
    const void *recipient;
    struct a733_gpu_resource_rail *destination;
    unsigned stage, failed, armed;
    /* GPU performance mode only: retain one exclusive DCDC4 lease from
     * initial acquisition; expected_uv follows its serialized writer. */
    unsigned gpu_performance;
    uint32_t expected_uv;
};
int a733_pmic_consumer_init(struct a733_pmic_consumer *,const struct a733_pmic_consumer_config *);
int a733_pmic_consumer_storage(struct a733_pmic *,const void *,size_t);
/* Before acquisition/concurrency; DCDC4 only. No provider access. */
int a733_pmic_consumer_gpu_performance(struct a733_pmic_consumer *);
/* GPU owner only, before a serialized write/readback operation. End the old
 * completed sample, retain its exclusive parent, and capture one new deadline
 * and expected voltage. Then sample after any write has settled. No retry or
 * lease release after uncertainty. 1 ready, 0 contention, -1 retained fault. */
int a733_pmic_consumer_gpu_begin(struct a733_pmic_consumer *,uint64_t,uint32_t expected_uv);
/* 1 ready, 0 pending/contention, -1 retained failure. Reserve performs only
 * ledger operations. Sample queues actual bus work for the separate provider
 * task. Both use one absolute acquisition deadline. */
int a733_pmic_consumer_reserve(struct a733_pmic_consumer *,uint64_t);
int a733_pmic_consumer_sample(struct a733_pmic_consumer *,uint64_t);
/* End only the completed request, preserving the stable parent lease; the next
 * sample reads again. Caller establishes any required settling interval. */
int a733_pmic_consumer_refresh(struct a733_pmic_consumer *,uint64_t);
/* Actual provider, request/borrow and parent authentication, outside GPU locks.
 * Health remains meaningful after the acquisition deadline has passed. */
int a733_pmic_consumer_health(struct a733_pmic_consumer *);
/* Arm immediately before manager acquisition; take consumes that authorization
 * by copying raw immutable bytes only. No provider/ledger callback runs inside
 * take. Recipient is the one native adapter bound before concurrency. */
int a733_pmic_consumer_arm(struct a733_pmic_consumer *,uint64_t);
int a733_pmic_consumer_take(struct a733_pmic_consumer *,const void *,uint64_t,
    struct a733_gpu_resource_rail *);
#endif
