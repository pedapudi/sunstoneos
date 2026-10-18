/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUNSTONE_A733_HDMI_NATIVE_H
#define SUNSTONE_A733_HDMI_NATIVE_H
#include "a733_hdmi.h"
#include "a733_pmic.h"
#include "a733_gpu_resources.h"
struct a733_platform_locks;
enum a733_hdmi_recovery_mode { A733_HDMI_INDEPENDENT_RECOVERY=0,
    A733_HDMI_SUPERVISED_PATTERN=1 };
/* Image-owned immutable boot admission. The image establishes sole control of
 * SYSCTRL resistor words0x160/164, all VO1 consumers, remote writers and prior
 * output before constructing this object. epoch identifies that actual cold
 * handoff. Independent mode uses recovery; supervised mode uses supervision
 * and retains all ownership until manual power removal, even if output cannot
 * be stopped. Supervised mode admits only the non-DMA 720p60 limited-range
 * TV0 pattern. See docs/a733-smp-supervised.md for the RAM-only recovery policy.
 * Neither mode establishes drain or permits resource reuse. initial must
 * identify a qualified complete analog image, including charge-pump provenance.
 * No built-in profile or default epoch is provided. These physical facts cannot
 * be established by a software token or by reading the initial register image.
 * The native image must refuse construction when any of this input is absent.
 */
struct a733_hdmi_native_config {
    struct a733_clock_leases *clocks;
    struct a733_rail_leases *rails;
    struct a733_pmic *pmic;
    struct a733_platform_locks *locks;
    struct a733_gpu_resource_manager *pck;
    unsigned pck_client;
    void *task;
    uint64_t owner,epoch,recovery;
    struct a733_hdmi_request request;
    enum a733_hdmi_recovery_mode recovery_mode;
    uint64_t supervision;
};
struct a733_hdmi_native {
    struct a733_hdmi_native *self;
    struct a733_hdmi_native_config config,sealed;
    struct a733_hdmi controller;
    struct a733_hdmi_config controller_config;
    struct a733_gpu_resource_lease pck_lease;
    struct a733_gpu_power_resources pck_ready;
    struct a733_clock_token clock;
    struct a733_rail_token rail;
    struct a733_pmic_lifetime pmic_lifetime;
    struct a733_pmic_request requests[2];
    struct a733_pmic_snapshot supplies[2];
    struct a733_hdmi_reservation grant;
    uint64_t deadline,last_now,power_deadline,reset_until;
    unsigned stage,step,domain_on,failed,held,clock_retained,rail_retained;
};
/* Initialize once before task scheduling. All managers, config and object are
 * disjoint, private and platform-lived. The designated task acquires PCK through
 * the actual resource manager and retains that lease for platform lifetime.
 * A single irreversible boot owner excludes other resistor-word writers. No
 * calls write hardware here. One designated EL1 task subsequently uses the
 * embedded controller's prepare/poll/start/service/stop APIs. PMIC work runs in
 * its separate provider task; a completed request stays borrowed for lifetime.
 * Native preparation authenticates actual tokens, reads actual PMIC supplies,
 * observes Device translations and programs source-defined VO1 PCK/CCU state.
 * It supplies no sink, drain, recovery or analog qualification success.
 */
int a733_hdmi_native_init(struct a733_hdmi_native *,const struct a733_hdmi_native_config *);
#endif
