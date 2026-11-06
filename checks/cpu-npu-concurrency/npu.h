#ifndef CPU_NPU_HOST_NPU
#define CPU_NPU_HOST_NPU
#include "task.h"
#include <stdint.h>
#include <stddef.h>
struct a733_npu_ops { int (*read)(void *,unsigned,uint32_t *); };
enum { A733_NPU_RUNNING=3 };
struct npu_service {
    struct { unsigned state;struct { void *token; } job; } controller;
    struct { struct { uint64_t ticket; } wait; } member[8];
    struct a733_npu_ops ops;
    void *cookie;unsigned running;uint64_t trigger_ticks;
    TaskHandle_t worker;
};
struct npu_runtime { struct npu_service service;struct { unsigned model; } slot[8]; };
struct os_npu_vector { const uint8_t *data;size_t bytes; };
extern const struct os_npu_vector os_npu_inputs[],os_npu_references[];
extern const unsigned os_npu_input_count,os_npu_output_count;
int npu_service_observe(struct npu_service *,void (*)(void *,const struct npu_service *,unsigned),void *);
unsigned npu_service_irq_completions(const struct npu_service *);
int os_npu_observed(void);
#endif
