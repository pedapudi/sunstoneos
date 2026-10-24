#ifndef SUNSTONE_A733_PCIE_NVME_H
#define SUNSTONE_A733_PCIE_NVME_H
#include "a733_pcie_function.h"
#define A733_PCIE_NVME_BYTES 4096u
#define A733_PCIE_NVME_DEPTH 2u /* One command outstanding per queue. */
/* Queue and data pool: admin SQ and CQ, I/O SQ and CQ, identify and data
 * pages. It must lie below 4 GiB, because the root port's inbound address
 * width is unpublished. */
#define A733_PCIE_NVME_POOL_BYTES (6u*4096u)
#define A733_PCIE_NVME_CLASS UINT32_C(0x010802) /* Mass storage, NVM, NVMe I/O. */
struct a733_pcie_nvme_config {
    uint64_t identity,timeout_us; /* timeout_us bounds each command and the shutdown. */
    uint16_t vendor,device;
    unsigned function,irq,vectors;
    uint32_t namespace_id;
};
/* Copied controller and namespace identity; strings are not terminated. */
struct a733_pcie_nvme_identity {
    uint16_t vendor,subsystem_vendor;
    uint32_t version,namespaces;
    uint64_t blocks;
    unsigned lba_shift,mdts;
    char serial[20],model[40],firmware[8];
};
/* Fixed read-only NVM command-set consumer of one PCIe function, adapted from
 * the pinned U-Boot polled driver (drivers/nvme/nvme.c): one admin and one
 * I/O queue pair, both two entries deep, and one 4096-byte read at a time.
 * The driver builds each command from its own state, and the BAR0 write rule
 * admits a submission doorbell only when the queued entry equals that exact
 * command, so no write, format or vendor command can be submitted. Completion
 * is the phase-tagged queue entry, never an interrupt. The controller must
 * start disabled. Initialization refuses queue and data memory that ends above
 * DMA_ADDRESS_32BIT_LIMIT, before any configuration or BAR access. After any
 * fault, and after orderly stop, the queue and data memory stays retained. */
struct a733_pcie_nvme {
    struct a733_pcie_nvme *self;
    struct a733_pcie_function function;
    struct device_memory_pool *pool;
    struct a733_pcie_nvme_config config;
    struct device_memory_lease lease;
    struct device_memory_extent memory; /* Admin SQ/CQ, I/O SQ/CQ, identify, data. */
    struct a733_pcie_nvme_identity identity;
    uint64_t deadline,lba,ready_us;
    uint32_t stride,events;
    uint16_t tail[2],head[2],command_id;
    uint8_t phase[2];
    unsigned stage,failed,pending,queue;
};
int a733_pcie_nvme_init(struct a733_pcie_nvme *,struct a733_pcie *,
    const struct a733_pcie_nvme_config *,struct device_memory_pool *,
    const struct a733_pcie_irq_ops *);
/* READY: idle with I/O queues; DONE data waits for copy. PENDING: progressing.
 * STOPPED: controller shut down, function stopped and port held in PERST. */
enum a733_pcie_result a733_pcie_nvme_poll(struct a733_pcie_nvme *);
enum a733_pcie_result a733_pcie_nvme_identity(struct a733_pcie_nvme *,struct a733_pcie_nvme_identity *);
/* Queue one aligned 4096-byte read inside the namespace. Returns 1 when accepted. */
int a733_pcie_nvme_read(struct a733_pcie_nvme *,uint64_t lba);
/* Copy a completed read out and return to READY. */
enum a733_pcie_result a733_pcie_nvme_copy(struct a733_pcie_nvme *,uint8_t[4096]);
/* Delete the I/O queues, request a normal controller shutdown, clear bus
 * mastering after shutdown completes, stop the function and then the port. */
int a733_pcie_nvme_shutdown(struct a733_pcie_nvme *);
/* Exact BAR0 write rule supplied to the function layer and native filter. */
int a733_pcie_nvme_write_allowed(const void *,unsigned bar,uint32_t offset,uint32_t value);
#endif
