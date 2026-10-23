#ifndef SUNSTONE_A733_PCIE_FUNCTION_H
#define SUNSTONE_A733_PCIE_FUNCTION_H
#include "a733_pcie.h"
#include "device_memory_pool.h"
/* Interrupt delivery for the claimed function. POLL keeps INTx disabled in the
 * command register and MSI-X disabled; the driver observes completion in
 * memory. INTX unmasks the function's one SII assert bit, delivered on
 * INTID184. MSIX programs up to four table entries to the DesignWare MSI
 * target, delivered on INTID185. An interrupt never establishes completion. */
enum a733_pcie_irq_mode { A733_PCIE_IRQ_POLL, A733_PCIE_IRQ_INTX, A733_PCIE_IRQ_MSIX };
#define A733_PCIE_MSIX_VECTORS 4u
#define A733_PCIE_D3_WAIT_US 10000u /* PCI PM D0<->D3hot recovery time. */
struct a733_pcie_function_id {
    uint16_t vendor,device;
    uint32_t class_revision;
    uint8_t header_type,interrupt_pin,pcie_cap,msi_cap,msix_cap,pm_cap;
};
/* One assigned memory BAR: identical CPU and PCI address inside region one.
 * flags keeps the BAR's low type bits; bytes is zero for an unused slot. */
struct a733_pcie_bar { uint32_t base,bytes,flags; };
/* Optional native GIC glue: arm 0 pending, 1 armed, -1 fault; event returns
 * notifications since the last call; stop is permanent closure. */
struct a733_pcie_irq_ops {
    int (*arm)(void *);
    int (*event)(void *,uint32_t *);
    void (*stop)(void *);
    void *cookie;
};
struct a733_pcie_function_config {
    uint64_t identity;
    uint16_t vendor,device;
    uint32_t class_code; /* Base class, subclass and programming interface. */
    unsigned function,irq,vectors;
    /* The driver's exact rule for every BAR write outside the MSI-X table.
     * The native write filter calls it with the driver state below. */
    int (*write_allowed)(const void *driver,unsigned bar,uint32_t offset,uint32_t value);
    const void *driver;
};
/* One trusted endpoint function behind the direct x1 port, not a PCI bus
 * framework. Bridges and switches are refused. All functions are scanned and
 * must be cold; only the claimed one is configured. Its memory BARs are placed
 * at natural alignment from the bottom of the 32-bit aperture, below 4 GiB, and
 * one identity region-one window covers them. I/O BARs stay unassigned with
 * I/O decode off. Disabled inbound ATU is identity forwarding, not DMA
 * isolation: the driver owns DMA memory and quiescence. */
struct a733_pcie_function {
    struct a733_pcie_function *self;
    struct a733_pcie *port;
    struct device_memory_pool *pool;
    struct a733_pcie_function_config config;
    struct a733_pcie_irq_ops irq;
    struct device_memory_lease msi_lease; /* MSI target page, never reused. */
    struct device_memory_extent msi_memory;
    struct a733_pcie_function_id ids[8];
    struct a733_pcie_bar bars[6];
    uint64_t wait_until;
    uint32_t window_bytes,command,sii,msix_table,msix_pba,msix_entries;
    uint16_t msix_control,pmcsr;
    uint8_t present,io,upper,msix_bar,pba_bar;
    unsigned stage,failed,locked,master,configured,soft_reset,notifications;
};
int a733_pcie_function_init(struct a733_pcie_function *,struct a733_pcie *,
    const struct a733_pcie_function_config *,struct device_memory_pool *,
    const struct a733_pcie_irq_ops *);
/* Advance claim, scan, BAR assignment, window, interrupt setup, power
 * transitions and stop. READY: idle in D0. PENDING: progressing or suspended.
 * STOPPED: orderly stop complete. RETAINED: every resource stays held. */
enum a733_pcie_result a733_pcie_function_poll(struct a733_pcie_function *);
/* Driver session in READY/D0. begin takes the port lock, checks the command
 * register and collects interrupt events (MSI-X vector bits, or bit 0 for
 * INTx). Every BAR access and master change belongs inside begin/end. end(0)
 * retains the port; end(1) releases the lock. */
enum a733_pcie_result a733_pcie_function_begin(struct a733_pcie_function *,uint64_t deadline,uint32_t *events);
int a733_pcie_function_read(struct a733_pcie_function *,unsigned bar,uint32_t offset,uint32_t *);
int a733_pcie_function_write(struct a733_pcie_function *,unsigned bar,uint32_t offset,uint32_t);
/* Bus mastering. Enable only after the driver owns every DMA target. Clear
 * only after the driver has proven the endpoint quiescent. */
int a733_pcie_function_master(struct a733_pcie_function *,int enable);
enum a733_pcie_result a733_pcie_function_end(struct a733_pcie_function *,int ok);
/* Requests advanced by poll. Each requires bus mastering clear. Suspend
 * enters D3hot and the link may enter L1; resume returns to D0 and restores
 * BARs, command and MSI-X when the function reports a soft reset. Stop masks
 * interrupts, clears decode and closes the windows; then a733_pcie_stop may
 * hold the endpoint in PERST. Returns 1 when accepted. */
int a733_pcie_function_suspend(struct a733_pcie_function *);
int a733_pcie_function_resume(struct a733_pcie_function *);
int a733_pcie_function_stop(struct a733_pcie_function *);
/* Exact lower native MMIO policy, active only after the core's one-way claim. */
int a733_pcie_function_native_address(const struct a733_pcie *,uintptr_t,unsigned,unsigned);
int a733_pcie_function_native_write(const struct a733_pcie *,uintptr_t,unsigned,uint32_t);
#endif
