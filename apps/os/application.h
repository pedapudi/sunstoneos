#ifndef SUNSTONE_OS_APPLICATION_H
#define SUNSTONE_OS_APPLICATION_H
#include "sunstone_app.h"
#include "sunstone_session.h"
#include "FreeRTOS.h"
#include "task.h"
/* Immutable image binding. Only the generated image unit names an app. */
struct ss_application {
    const struct ss_manifest *manifest;
    void (*init)(void);
    void (*main)(const struct ss_grant *);
    StackType_t *stack;
    StaticTask_t *task;
};
extern const struct ss_application ss_application;
extern uint8_t ss_result_record[];
extern const size_t ss_result_capacity;
struct a733_pin_handoff;
struct os_application {
    const char *name;
    int (*task_privileged)(void (*)(void *),void *);
    int (*pins)(struct a733_pin_handoff *);
    int (*start)(void);
};
extern const struct os_application os_applications[];
extern const unsigned os_application_count;
int os_application_task_privileged(void (*)(void *),void *);
int os_application_pins(struct a733_pin_handoff *);
int os_application_start(void);
int os_application_prepare(void);
int os_application_deadline(void);
unsigned os_application_activity_generation(void);
int os_application_finished(void);
void os_application_usb_ready(void);
void os_application_host_observed(int);
int os_application_export_live(void);
/* USB0 owner only: one request until its reply has completed on USB. */
int os_host_offer(const struct ss_host_request *);
int os_host_reply(struct ss_host_reply *,uint8_t [SS_HOST_PAYLOAD_BYTES]);
void os_host_delivered(void);
void os_storage_service(void);
int os_capture_start(void);
int os_audio_client_admit(uint32_t);
#endif
