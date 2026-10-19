#ifndef SUNSTONE_UART_DMA_PRIVATE_H
#define SUNSTONE_UART_DMA_PRIVATE_H
#include "a733_dma_startup.h"
#include "a733_dma_native.h"
#include "a733_memory_reservations.h"
#include "dma_service.h"
struct uart_dma_application {
 struct a733_dma_startup startup;
 struct a733_dma_native native;
 struct dma_service service;
 struct device_memory_pool *pool;
 struct device_memory_lease lease;
 struct device_memory_extent extent;
 struct device_memory_allocation allocation;
 struct dma_service_job jobs[2];
 struct a733_dma_segment segments[2];
 struct a733_dma_mapping mappings[2];
 struct dma_service_reply replies[2];
 uint64_t pending,job_tickets[2],stop_deadline;
 unsigned active,operation,job,closed,requests_disabled,drained;
 unsigned polls[2];
 unsigned failing,activated,cancel_job,cancel_operation,uncertain;
};
extern struct uart_dma_application uart_dma;
int uart_dma_construct(void);
int uart_dma_step(void);
#endif
