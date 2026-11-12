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
/* Generated: one span per manifest region, the table behind ss_grant()->regions. */
extern struct ss_span ss_grant_spans[];
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
struct pvr_compute_queue;
int os_application_gpu_share_bind(struct pvr_compute_queue *);
int os_application_deadline(void);
unsigned os_application_activity_generation(void);
int os_application_finished(void);
int os_application_succeeded(void);
/* Wake the main task from its OS wait, or keep the wake for its next wait (apps/os/application.c). */
void os_application_wake_main(void);
/* The same wake-up for the task that submitted a request, from a task or from an interrupt handler. */
void os_application_wake_task(TaskHandle_t);
void os_application_wake_task_from_isr(TaskHandle_t,long *woken); /* a BaseType_t pointer */
/* The OS wait of the calling task to an absolute time in microseconds (sunstone_deadline_now_us epoch), ended early by a
 * wake-up or when unless() returns nonzero after the wait is registered. */
int os_application_wait_unless(uint64_t until,int (*unless)(void));
/* The low 32 bits of the return address of the application's latest ss_progress call (0 before the first): where its
 * waiting loop is. The cutoff failure of the return supervisor records it. */
uint32_t os_application_progress_site(void);
/* Publish the failure record as the session result (status SS_RESULT_FAILED)
 * and mark the application finished. The caller holds the failure return. */
void os_application_failed(const struct ss_failure *,uint64_t epoch,uint64_t generation);
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
