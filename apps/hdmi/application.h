#ifndef SUNSTONE_HDMI_APPLICATION_H
#define SUNSTONE_HDMI_APPLICATION_H
#include "session.h"
#include "boot.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_pins_native.h"
#include "a733_pmic_native.h"
#include "task.h"
#if SUNSTONE_HDMI_AUDIO
#include "../hdmi-audio/native.h"
#endif
#if SUNSTONE_HDMI_AUDIO_USB
#include "../hdmi-audio/usb.h"
#include "../hdmi-audio/observation.h"
#define HDMI_TASK_COUNT 3
#else
#define HDMI_TASK_COUNT 2
#endif
enum hdmi_application_state { HDMI_APP_PENDING,HDMI_APP_ACTIVE,
    HDMI_APP_STOPPED_RETAINED,HDMI_APP_FAULT_RETAINED };
/* Atomic status is diagnostic only; ACTIVE means controller programming and
 * health observations passed. Only a person/monitor can establish visible output. */
struct hdmi_application {
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
    StaticTask_t tasks[HDMI_TASK_COUNT];StackType_t stacks[HDMI_TASK_COUNT][2048];TaskHandle_t handles[HDMI_TASK_COUNT];
    atomic_uint fatal,pmic_ready,state,cancel;
    uint64_t deadline,stop_at,last_now;
    unsigned stage;
};
extern struct hdmi_application app;
int hdmi_live(void *);
void hdmi_fail(void *);
int hdmi_output_step(void);
void hdmi_output_task(void *);
#endif
