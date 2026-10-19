#ifndef SUNSTONE_A733_PCIE_EPF_TEST_H
#define SUNSTONE_A733_PCIE_EPF_TEST_H
#include "a733_pcie.h"
#include "device_memory_pool.h"
#define A733_PCIE_TEST_WINDOW UINT32_C(0x22000000)
#define A733_PCIE_TEST_WINDOW_BYTES UINT32_C(0x100000)
#define A733_PCIE_TEST_BYTES 4096u
#define A733_PCIE_TEST_ALLOCATION 12288u
struct a733_pcie_epf_config {
    uint64_t identity,timeout_us;
    uint16_t vendor,device;
    unsigned bar;
};
struct a733_pcie_epf_irq {
    int (*arm)(void *);
    int (*event)(void *,uint32_t *);
    void (*stop)(void *);
    void *cookie;
};
/* Fixed trusted pci-epf-test consumer, not a PCI bus framework. The admitted
 * endpoint DMA/controller must complete COPY before removing endpoint mappings
 * and make writes visible before status/MSI. Disabled inbound ATU is identity
 * forwarding, not DMA isolation. No command or memory allocation is reused. */
struct a733_pcie_epf_test {
    struct a733_pcie_epf_test *self;
    struct a733_pcie *port;
    struct device_memory_pool *pool;
    struct a733_pcie_epf_config config;
    struct a733_pcie_epf_irq irq;
    struct device_memory_lease leases[3];
    struct device_memory_extent memory[3];
    uint64_t operation_deadline,bar_bytes;
    uint32_t command,msi_control,status,event_count;
    unsigned stage,failed,published,msi64,bar64,claimed,submitted;
};
int a733_pcie_epf_init(struct a733_pcie_epf_test *,struct a733_pcie *,
    const struct a733_pcie_epf_config *,struct device_memory_pool *,
    const struct a733_pcie_epf_irq *);
enum a733_pcie_result a733_pcie_epf_poll(struct a733_pcie_epf_test *);
/* Owner-task copied output only. All allocations and MMIO authority remain
 * retained after this copy. A software timeout cannot establish DMA drain. */
enum a733_pcie_result a733_pcie_epf_copy(struct a733_pcie_epf_test *,uint8_t[4096]);
void a733_pcie_epf_stop(struct a733_pcie_epf_test *);
/* Exact lower native MMIO policy, active only after the core's one-way claim. */
int a733_pcie_epf_native_address(const struct a733_pcie *,uintptr_t,unsigned,unsigned);
int a733_pcie_epf_native_write(const struct a733_pcie *,uintptr_t,unsigned,uint32_t);
#endif
