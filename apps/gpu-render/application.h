#ifndef SUNSTONE_GPU_APPLICATION_PRIVATE_H
#define SUNSTONE_GPU_APPLICATION_PRIVATE_H
#include "a733_board_application.h"
#include "a733_native_usb.h"
#include "a733_gpu_backend.h"
#include "a733_gpu_graphics_memory.h"
#include "a733_gpu_platform_native.h"
#include "cached.h"
struct gpu_application {
    struct a733_board_session session;
    struct a733_native_usb shared;
    struct a733_gpu_platform_native native;
    struct a733_gpu_platform power;
    struct a733_gpu_backend backend;
    struct a733_gpu_reserved firmware;
    struct a733_gpu_compute_reserved compute;
    struct a733_gpu_graphics_memory graphics;
    struct pvr_gpu_service service;
    StaticTask_t tasks[3];
    StackType_t stacks[3][4096];
    TaskHandle_t handles[3];
    atomic_uint fatal,result,usb_observed,preparation_failed;
#ifdef SUNSTONE_GPU_RUNTIME_INPUT
    /* USB writes only while idle, client consumes after acquire. The slot stays
     * owned until completion/rejection; USB output copies stop while pending. */
    atomic_uint runtime_state, runtime_sequence, runtime_closed;
    uint32_t runtime_operation, runtime_expected, runtime_bytes;
    unsigned char runtime_input[PVR_COMPUTE_PACKAGE_INPUT];
#endif
    struct pvr_gpu_service_reply last;
    uint32_t pixels[4096];
    uint32_t compute_words[2][PVR_COMPUTE_JOB_WORDS], compute_sequence[2];
};
extern struct gpu_application app;
const struct a733_board_session *gpu_application_session(void);
int gpu_application_live(void *);
void gpu_application_fail(void *);
void gpu_application_usb(void *);
#endif
