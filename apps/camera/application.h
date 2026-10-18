#ifndef SUNSTONE_CAMERA_APPLICATION_H
#define SUNSTONE_CAMERA_APPLICATION_H
#include "session.h"
#include "boot.h"
#include "a733_maintenance_watchdog_internal.h"
#include "task.h"
#define CAMERA_FRAME_BYTES (2u*A733_CAMERA_NATIVE_PLANE_BYTES)
struct camera_application {
    struct camera_session admitted,session;
    struct a733_native_usb shared;
    struct a733_media_clock media;
    struct a733_camera_native camera;
    struct a733_rail_token rail;
    struct device_memory_lease output;
    struct device_memory_extent pixels;
    struct a733_watchdog_window window;
    StaticTask_t tasks[3];StackType_t stacks[3][2048];TaskHandle_t handles[3];
    atomic_uint fatal,result,cancel;
    unsigned stage,held;
};
extern struct camera_application app;
int camera_live(void *);
void camera_fail(void *);
void camera_usb0_task(void *);
void camera_capture_task(void *);
int camera_capture_step(void);
/* USB task reads only after release/acquire publication of complete output. */
int camera_output(unsigned offset,void *,unsigned bytes);
#endif
