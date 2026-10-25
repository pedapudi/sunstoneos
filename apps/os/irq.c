/* The image's fixed shared-peripheral interrupt table. The EL3 boot programs
 * these routes (boot.patch) and the shared handler dispatches through them;
 * every other INTID keeps the status-4 stop. Device owners enable sources. */
#include "irq_table.h"
#include "a733_thermal.h"
void os_thermal_irq(void);
#ifdef SUNSTONE_A733_OS_NPU
#include "npu.h"
#endif
#ifdef SUNSTONE_A733_OS_AUDIO
void os_audio_irq(void);
#endif
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
#include "dispatch.h"
#endif
const struct a733_irq_route a733_irq_routes[]={
    {A733_THERMAL_INTID,0,0xd8,os_thermal_irq},
#ifdef SUNSTONE_A733_OS_NPU
    {97,OS_NPU_CORE,0xd8,os_npu_irq},
#endif
#ifdef SUNSTONE_A733_OS_AUDIO
    /* DMA0 (A733_DMA0_INTID), on the core of the audio DMA worker. */
    {143,0,0xd8,os_audio_irq},
#endif
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    /* GPU (A733_GPU_INTID, SPI 63), on the core of the recognizer and the GPU worker, so a
     * completion wakes the recognizer without an inter-processor interrupt. */
    {95,OS_RECOGNITION_CORE,0xd8,a733_gpu_mimus_irq},
#endif
};
const unsigned a733_irq_route_count=sizeof(a733_irq_routes)/sizeof(a733_irq_routes[0]);
