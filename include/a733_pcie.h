#ifndef SUNSTONE_A733_PCIE_H
#define SUNSTONE_A733_PCIE_H
#include "a733_pmic.h"

enum a733_pcie_result { A733_PCIE_PENDING, A733_PCIE_READY, A733_PCIE_BUSY,
    A733_PCIE_INVALID, A733_PCIE_RETAINED };
/* This profile requires an authenticated V1.2 board/endpoint handoff: external
 * endpoint power is already stable, PD22 is its active-low PERST and starts
 * asserted, PD23 is not driven, and no endpoint has inherited DMA ownership.
 * The caller excludes secure/remote writers, GIC handlers and interrupt-route
 * writers for INTIDs168–175/184/185, and all COMB1 USB3 users. Shared
 * SERDES100MHz and PCK7 are already ON. No global reset or rail write occurs.
 * Identity-mapped Device apertures and reviewed operation-specific recovery
 * are prerequisites: qualified independent reset, or supervised permanent
 * retention until confirmed whole-board power removal. Software deadlines
 * cannot recover stalled MMIO; the physical recovery path must be qualified before use.
 * deadline bounds software progress, not an unresponsive physical bus access.
 */
struct a733_pcie_config {
    uint64_t owner, identity, topology, deadline, lifetime_deadline;
    unsigned pck_client;
    struct a733_pin_config inherited[4]; /* PD20..23; PD22 GPIO output low. */
    uint32_t fabric[3]; /* Exact AHB/NSI/MBUS selectors/divisors/gates. */
    uint32_t fabric_hz[3];
};
struct a733_pcie_ops {
    int (*now)(void *,uint64_t *);
    int (*current)(void *);
    int (*admit)(void *,const struct a733_pcie_config *);
    int (*enter)(void *,uint64_t);
    void (*leave)(void *);
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,unsigned width,uint32_t *);
    int (*write)(void *,uintptr_t,unsigned width,uint32_t);
    int (*outside)(void *,const void *,size_t);
};
struct a733_pcie_endpoint {
    uint16_t vendor, device, command, link_status;
    uint32_t class_revision;
    uint8_t header_type, pcie_cap, msi_cap;
};
/* Private fixed storage, immutable after initialization except through this
 * API. Every dependency has platform lifetime; no copy, rebind or reinit.
 * One designated task advances it. Application configuration reads serialize
 * the same outbound window and cannot write endpoint command/BAR registers.
 */
struct a733_pcie_epf_test;
struct a733_pcie {
    struct a733_pcie *self;
    struct a733_pcie_config config;
    struct a733_pcie_ops ops;
    void *cookie;
    struct a733_clock_leases *clocks;
    struct a733_pins *pins;
    struct a733_rail_leases *rails;
    struct a733_pmic *pmic;
    struct a733_gpu_resource_manager *pck;
    struct a733_clock_token clock;
    struct a733_rail_token rail;
    struct a733_pin_lease_banked pin;
    struct a733_gpu_resource_lease pck_lease;
    struct a733_pmic_lifetime pmic_lifetime;
    struct a733_pmic_request request;
    /* DCDC2 core0.8V, then BLDO1 PD-bank1.8V; analog routing remains
     * separately authenticated for the fitted board. No BLDO2 inference. */
    struct a733_pmic_snapshot supplies[2];
    struct a733_pcie_endpoint endpoint;
    atomic_uint busy;
    uint64_t deadline,last_now,wait_until,perst_since;
    unsigned phase,phy_index,supply_index,held,failed,clock_retained,rail_retained;
    uint8_t root_cap;
    uint32_t last_value;
    struct a733_pcie_epf_test *test; /* One-way concrete test ownership. */
};
/* Pure initialization. One registry/rail ledger/PCK manager/pin manager must
 * cover every overlapping local and remote writer. Selected parents are held
 * explicitly. Successful snapshots remain borrowed from this same held rail
 * lifetime; they establish programmed values, not electrical measurements.
 */
int a733_pcie_init(struct a733_pcie *,const struct a733_pcie_config *,
    const struct a733_pcie_ops *,void *,struct a733_clock_leases *,struct a733_pins *,
    struct a733_rail_leases *,struct a733_pmic *,struct a733_gpu_resource_manager *);
enum a733_pcie_result a733_pcie_poll(struct a733_pcie *);
/* READY publishes copied direct endpoint0:0 identity with BME observed clear.
 * Cold endpoint compliance is trusted; disabled inbound iATU is NOT isolation.
 * No DMA, MSI activation, hotplug, bridge enumeration or memory reclamation is
 * authorized by this initial profile. Failures keep every lease and any held
 * register mutex. There is no automatic rollback/reset or close API.
 */
enum a733_pcie_result a733_pcie_identity(struct a733_pcie *,struct a733_pcie_endpoint *);
/* Read one aligned dword in the direct endpoint's 4KiB config space. Function0
 * is always valid; others require the observed multifunction header. Link,
 * resource health and absolute deadline are checked before/after access.
 * Output remains unchanged on refusal/uncertainty. All public outputs must be
 * disjoint from the complete provider/manager/adapter object graph.
 */
enum a733_pcie_result a733_pcie_config_read(struct a733_pcie *,unsigned function,
    unsigned offset,uint64_t deadline,uint32_t *);

int a733_pcie_output_outside(const struct a733_pcie *,const void *,size_t);
/* Internal concrete test-client boundary. Claim reestablishes function zero
 * and cold endpoint state under the existing mutex. No default getter/poll can
 * advance a claimed port. Lock success keeps busy+register ownership through
 * test_end; fault retains resources and any held register mutex. */
enum a733_pcie_result a733_pcie_test_claim(struct a733_pcie *,struct a733_pcie_epf_test *);
enum a733_pcie_result a733_pcie_test_lock(struct a733_pcie *,struct a733_pcie_epf_test *,uint64_t);
enum a733_pcie_result a733_pcie_test_end(struct a733_pcie *,int);
/* Internal driver interface: callers already own the common register mutex. */
int a733_pcie_read(struct a733_pcie *,uintptr_t,unsigned,uint32_t *);
int a733_pcie_write(struct a733_pcie *,uintptr_t,unsigned,uint32_t);
int a733_pcie_phy_step(struct a733_pcie *); /* 1 complete,0 pending,-1 fault. */
int a733_pcie_phy_address(uintptr_t,unsigned,unsigned writing);
#endif
