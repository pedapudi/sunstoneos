#ifndef SUNSTONE_PCIE_PRIVATE_H
#define SUNSTONE_PCIE_PRIVATE_H
#include "pcie_session.h"
#include "../board/application.h"
#include "a733_pcie_native.h"
#include "a733_pcie_nvme.h"
struct pcie_application {
 struct pcie_session session;
 struct a733_pcie port;
 struct a733_pcie_native native;
 struct a733_pcie_config config;
 struct a733_pcie_ops operations;
 struct a733_pcie_nvme nvme;
 struct a733_pcie_nvme_identity identity;
 uint8_t data[A733_PCIE_NVME_BYTES];
 StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
 atomic_uint result,failed;
 unsigned stage,retaining,clock_retained,rail_retained,window_checked;
 uint8_t payload[PCIE_OUTPUT_BYTES],digest[32],session_digest[32];
};
extern struct pcie_application pcie;
int pcie_construct(void);
int pcie_step(void);
void pcie_task(void *);
void pcie_cancel(void);
int pcie_output_ready(void);
int pcie_live(void);
int pcie_publish(void);
/* Device mapping of the claimed function's region-one window, checked once
 * after BAR assignment and before the first BAR access. */
int pcie_window_mapped(uint32_t bytes);
struct device_memory_pool *pcie_nvme_pool(void);
#endif
