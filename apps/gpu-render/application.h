#ifndef SUNSTONE_GPU_APPLICATION_PRIVATE_H
#define SUNSTONE_GPU_APPLICATION_PRIVATE_H
#include "a733_board_application.h"
#include "a733_native_usb.h"
#include "a733_gpu_backend.h"
#include "a733_gpu_graphics_memory.h"
#include "a733_gpu_platform_native.h"
#include "cached.h"
#ifdef SUNSTONE_GPU_LIFECYCLE
#include "a733_gpu_disposal.h"
#include "pvr_gpu_lifecycle.h"
#define GPU_TEXTURE_HEADER_BYTES 72u
#else
#define GPU_TEXTURE_HEADER_BYTES 64u
#endif
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
#ifdef SUNSTONE_GPU_LIFECYCLE
    struct a733_gpu_disposal disposal;
    atomic_uint_fast64_t runtime_epoch;
    uint64_t runtime_request_epoch;
#endif
    StaticTask_t tasks[3];
    StackType_t stacks[3][4096];
    TaskHandle_t handles[3];
    atomic_uint fatal,result,usb_observed,preparation_failed;
#if defined(SUNSTONE_GPU_RUNTIME_INPUT) || defined(SUNSTONE_GPU_RUNTIME_DRAW)
    /* USB writes only while idle, client consumes after acquire. The slot stays
     * owned until completion/rejection; USB output copies stop while pending. */
    atomic_uint runtime_state, runtime_sequence, runtime_closed;
    uint32_t runtime_operation, runtime_expected, runtime_bytes;
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
    unsigned char runtime_input[PVR_RENDER_TEXTURE_CHUNK];
    struct pvr_render_texture_request texture_request;
    atomic_uint texture_tx_state;
    uint8_t texture_header[GPU_TEXTURE_HEADER_BYTES];
    const uint8_t *texture_tx_data;
    uint32_t texture_tx_bytes,texture_tx_header;
    uint64_t texture_generation;
#else
    unsigned char runtime_input[PVR_COMPUTE_PACKAGE_INPUT];
#endif
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
#ifdef SUNSTONE_GPU_LIFECYCLE
int gpu_application_lifecycle(unsigned operation);
#endif
#ifdef SUNSTONE_GPU_RUNTIME_TEXTURE
void gpu_application_texture_client(void);
void gpu_application_texture_closed(void);
void gpu_application_texture_usb(void);
void gpu_application_texture_tx(uint32_t);
#endif
#endif
