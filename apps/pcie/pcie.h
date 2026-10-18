#ifndef SUNSTONE_PCIE_PRIVATE_H
#define SUNSTONE_PCIE_PRIVATE_H
#include "pcie_session.h"
#include "../board/application.h"
#include "a733_pcie_native.h"
struct pcie_application {
 struct pcie_session session;
 struct a733_pcie port;
 struct a733_pcie_native native;
 struct a733_pcie_config config;
 struct a733_pcie_ops operations;
 struct a733_pcie_endpoint endpoint;
 StaticTask_t task;StackType_t stack[2048];TaskHandle_t handle;
 atomic_uint result,failed;
 unsigned stage,retaining,clock_retained,rail_retained,index,cap_next,cap_count;
 unsigned pcie_cap,msi_cap,msix_cap;
 uint64_t cap_seen;
 uint32_t header[4];
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
#endif
