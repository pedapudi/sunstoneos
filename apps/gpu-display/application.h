#ifndef SUNSTONE_GPU_DISPLAY_APPLICATION_H
#define SUNSTONE_GPU_DISPLAY_APPLICATION_H
#include "../hdmi/session.h"
#include "../hdmi/framebuffer.h"
#include "input.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_pins_native.h"
#include "a733_pmic_native.h"
#include "a733_gpu_backend.h"
#include "a733_gpu_graphics_memory.h"
#include "a733_gpu_platform_native.h"
#include "task.h"
#define GPU_DISPLAY_FRAME_BYTES UINT32_C(1847296)
#define GPU_DISPLAY_BANK_BYTES (2u*GPU_DISPLAY_FRAME_BYTES+8192u)
#define GPU_DISPLAY_PARENT_BYTES UINT32_C(4194304)
#define GPU_DISPLAY_FRAME_OFFSET UINT32_C(36864)
#define GPU_DISPLAY_STOP_RESERVE UINT64_C(2000000)
struct gpu_display_application {
    struct hdmi_session session;
    struct a733_counter_clock counter;
    struct a733_platform_locks locks;
    struct a733_clock_leases clocks;
    struct a733_pins_native pin_native;
    struct a733_pins pins;
    struct a733_gpu_resource_native pck_native;
    struct a733_gpu_resource_manager pck;
    struct a733_rail_leases rails;
    struct a733_pmic_native pmic_native;
    struct a733_pmic pmic;
    struct a733_hdmi_native hdmi;
    struct a733_gpu_platform_native gpu_native;
    struct a733_gpu_platform gpu_power;
    struct a733_gpu_backend gpu_backend;
    struct a733_gpu_reserved gpu_firmware;
    struct a733_gpu_compute_reserved gpu_compute;
    struct a733_gpu_graphics_memory gpu_graphics;
    struct pvr_gpu_service gpu_service;
    StaticTask_t tasks[3];StackType_t stacks[3][4096];TaskHandle_t handles[3];
    atomic_uint fatal,pmic_ready,frames_ready,gpu_closed,state,pmic_park_request,pmic_parked;
    uint64_t deadline,stop_at,last_now;
    uint32_t copied[2],frame_sequence[2],presented[3],presentations;
    unsigned stage;
};
extern struct gpu_display_application app;
/* Root authenticates the containing GPU/display authority and extended layout.
 * The original HDMI provider fields remain immutable and profile-specific. */
const struct hdmi_session *gpu_display_hdmi_get(void);
int hdmi_live(void *);
void hdmi_fail(void *);
int gpu_display_render_prepare(void);
void gpu_display_render_task(void *);
void gpu_display_irq(void);
int gpu_display_bank_copy(unsigned,uint32_t,uint32_t,const void *,uint32_t);
int gpu_display_bank_seal(void);
void gpu_display_output_task(void *);
int gpu_display_output_step(void);
BaseType_t gpu_display_task_privileged(TaskFunction_t,void *);
#endif
