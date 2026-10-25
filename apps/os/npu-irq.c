#include "npu.h"
#define NPU_IRQ_CORE OS_NPU_CORE
#define NPU_IRQ_PRIORITY 0xd8u
#define NPU_IRQ_HANDLER os_npu_irq
#define NPU_IRQ_EXTERNAL_BIND 1
#include "../npu-interrupt/irq.c"
int os_npu_irq_bind(struct npu_runtime *rt)
{
    if(!rt||!bind_ready())return 0;
    binding.masked=1;
    if(!npu_runtime_bind_irq(rt,&ops,&binding,UINT32_C(1)<<OS_NPU_CORE,1))return 0;
    binding.service=&rt->service;return 1;
}
