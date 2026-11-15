/* The OS services: the one declaration of each service that a session can grant, the record it carries, its entry in the
 * fixed start list and the tasks the supervisor watches. Everything else is derived from these rows:
 * - in C, the grant bits, whether the image links a service, the service ids and the supervision table (apps/os/os.h),
 *   the session decode of the records (apps/os/session.c), their wire size (apps/os/grants.c), the progress the
 *   supervisor requires (apps/os/services.c), the start list (written by tools/sunstone-manifest.py from the macros) and
 *   the image's deadline-owner slots (OS_DEADLINE_OWNERS);
 * - in Python, the record sizes, magics and versions of the host session builder (apps/os/host.py) and the grants and
 *   records that checks/os-session-grants.py covers, which read the rows of this file.
 * The numbers of the application ABI, the device bits and the session extent types, stay in include/sunstone_app.h and
 * include/sunstone_session.h; the rows name them. Adding a service is a row in each list that applies, plus its code:
 * the start entries, the record's struct in struct os_session and its validation in apps/os/admission.c.
 *
 * Python reads the rows as written: one X(...) per line, arguments separated by commas, no nested parentheses except in
 * a macro name the rows already use. This header declares no functions, so
 * host models can include it alone. */
#ifndef SUNSTONE_OS_SERVICES_H
#define SUNSTONE_OS_SERVICES_H

/* The build options that still select a service's code. OS_WITH_x(...) is its arguments when the option is on and
 * nothing when it is off; each goes when its option goes (docs/designs/one-os-image.md). */
#define OS_ALWAYS(...) __VA_ARGS__
#ifdef SUNSTONE_A733_OS_DMA
#define OS_WITH_DMA(...) __VA_ARGS__
#else
#define OS_WITH_DMA(...)
#endif
/* The headset is an object library that an image links when its application declares the headset device
 * (cmake/sunstone-image.cmake): SUNSTONE_A733_OS_HEADSET is defined for that library and for the image's own tables,
 * never for the OS core, which reaches the library through weak references. */
#ifdef SUNSTONE_A733_OS_HEADSET
#define OS_WITH_HEADSET(...) __VA_ARGS__
#else
#define OS_WITH_HEADSET(...)
#endif
/* The video decoder service likewise (SUNSTONE_A733_OS_VIDEO_DECODE): an image links it when its application declares
 * the video-decode device. */
#ifdef SUNSTONE_A733_OS_VIDEO_DECODE
#define OS_WITH_VIDEO_DECODE(...) __VA_ARGS__
#else
#define OS_WITH_VIDEO_DECODE(...)
#endif
#ifdef SUNSTONE_A733_OS_GPU
#define OS_WITH_GPU(...) __VA_ARGS__
#else
#define OS_WITH_GPU(...)
#endif
#ifdef SUNSTONE_TLBI_ISSUER
#define OS_WITH_TLBI(...) __VA_ARGS__
#else
#define OS_WITH_TLBI(...)
#endif
/* The write-capable image's SD0 owner task (apps/os/sd_owner.c, linked with SUNSTONE_A733_OS_SD_WRITE). */
#ifdef SUNSTONE_A733_OS_SD_WRITE
#define OS_WITH_SD_WRITE(...) __VA_ARGS__
#else
#define OS_WITH_SD_WRITE(...)
#endif

/* X(NAME, bit, with): a service that a session grants, its bit in the granted set (OS_GRANT_NAME) and the option that
 * links its code (OS_LINKED_NAME is 1 when it is linked). A session cannot grant a service whose code the image lacks.
 * The headset's code is linked per image, so the session decode tests a weak reference instead. */
#define OS_GRANT_ROWS(X) \
    X(NPU, 0, OS_ALWAYS) \
    X(AUDIO, 1, OS_ALWAYS) \
    X(CPU_CLOCK, 2, OS_ALWAYS) \
    X(DMA, 3, OS_WITH_DMA) \
    X(RADIO, 4, OS_ALWAYS) \
    X(HEADSET, 5, OS_ALWAYS) \
    X(USB0, 6, OS_ALWAYS)

/* X(GRANT, extent, field, bytes, magic, version, need): the record a granted service carries, as the session extent of
 * that type, copied to the field of struct os_session. A record that is present grants its service. need says when the
 * record must be present: 0 never (an optional service), OS_RECORD_LINKED whenever the image links the service, or a
 * device bit, which the session must hold exactly when the record is present. The radio has no record: the radio
 * device bit, with a radio workload in the board prefix, grants it (apps/os/session.c). */
#define OS_RECORD_LINKED 0xffffffffu
#define OS_RECORD_ROWS(X) \
    X(AUDIO, SS_EXTENT_AUDIO, audio, OS_AUDIO_SESSION_BYTES, OS_AUDIO_SESSION_MAGIC, OS_AUDIO_SESSION_VERSION, SS_DEVICE_AUDIO_I2S) \
    X(NPU, SS_EXTENT_NPU, npu, OS_NPU_SESSION_BYTES, OS_NPU_SESSION_MAGIC, OS_NPU_SESSION_VERSION, 0) \
    X(CPU_CLOCK, SS_EXTENT_CPU_GRANT, cpu_clock, OS_CPU_CLOCK_SESSION_BYTES, A733_CPU_CLOCK_GRANT_MAGIC, A733_CPU_CLOCK_GRANT_VERSION, 0) \
    X(DMA, SS_EXTENT_DMA, dma, OS_DMA_SESSION_BYTES, OS_DMA_SESSION_MAGIC, OS_DMA_SESSION_VERSION, OS_RECORD_LINKED)

/* X(name, prefix, start, with, owners): an entry of the fixed start list, in list order, before the application's own
 * (apps/os/start.c). The entry's functions are prefix_task_privileged, prefix_pins and start. owners is the number of
 * tasks the entry can create whose entry function registers a deadline owner (sunstone_deadline_register): the tasks its
 * start function creates, and the tasks those tasks create. The count assumes that each creation call creates one task.
 * checks/deadline-owners.py derives the same number from the linked code of each image and refuses a column that
 * differs. The audio and dma rows both count the DMA0 pool's two tasks (OS_DMA_POOL_OWNERS). The large row counts
 * none: its loader registers only through the read-only SD0 binding (OS_LARGE_SD_DEADLINES). The sd row's task, the
 * write-capable image's SD0 owner, registers when it opens the card for the first storage call. */
#define OS_START_ROWS(X) \
    X("thermal", os_thermal, os_thermal_start, OS_ALWAYS, 1) \
    X("large", os_large, os_large_start, OS_ALWAYS, 0) \
    X("tlbi", os_tlbi, os_tlbi_start, OS_WITH_TLBI, 1) \
    X("npu", os_npu, os_npu_start, OS_ALWAYS, 3) \
    X("audio", os_audio, os_capture_start, OS_ALWAYS, 3) \
    X("dma", os_dma, os_dma_start, OS_WITH_DMA, 2) \
    X("radio", os_radio, os_radio_start, OS_ALWAYS, 0) \
    X("headset", os_headset, os_headset_start, OS_WITH_HEADSET, 2) \
    X("i2c", os_i2c, os_i2c_start, OS_ALWAYS, 1) \
    X("rtc", os_rtc, os_rtc_start, OS_ALWAYS, 1) \
    X("g2d", os_g2d, os_g2d_start, OS_ALWAYS, 1) \
    X("video-decode", os_video_decode, os_video_decode_start, OS_WITH_VIDEO_DECODE, 1) \
    X("gpu-queue", os_gpu_queue, os_gpu_queue_start_entry, OS_WITH_GPU, 1) \
    X("sd", os_sd, os_sd_start, OS_WITH_SD_WRITE, 1)

/* X(NAME, name, window_us, grant, with): a task the supervisor watches, after the board's five and the application
 * (OS_SERVICE_NAME, in this order). It must report within window_us, and only when the session grants every service in
 * grant (0: every session). Every id is in every image; the headset's tasks exist only in an image that links the headset. */
#define OS_SUPERVISED_ROWS(X) \
    X(THERMAL, "thermal", 1000000, 0, OS_ALWAYS) \
    X(NPU, "npu", 5000000, OS_GRANT_NPU, OS_ALWAYS) \
    X(AUDIO, "audio", 1000000, OS_GRANT_AUDIO, OS_ALWAYS) \
    X(RADIO, "radio", 1000000, OS_GRANT_RADIO, OS_ALWAYS) \
    X(NETWORK, "network", 1000000, OS_GRANT_RADIO, OS_ALWAYS) \
    X(BLUETOOTH, "bluetooth", 1000000, OS_GRANT_HEADSET, OS_ALWAYS) \
    X(HEADSET, "headset", 1000000, OS_GRANT_HEADSET, OS_ALWAYS)

/* What the rows define. */
#define OS_GRANT_BIT_(NAME, bit, with) OS_GRANT_##NAME = 1u << (bit),
#define OS_GRANT_LINKED_(NAME, bit, with) OS_LINKED_##NAME = with(1) + 0,
enum { OS_GRANT_ROWS(OS_GRANT_BIT_) };
enum { OS_GRANT_ROWS(OS_GRANT_LINKED_) };
#define OS_SERVICE_ID_(NAME, name, window_us, grant, with) with(OS_SERVICE_##NAME,)
enum { OS_SERVICE_APPLICATION = 5, OS_SUPERVISED_ROWS(OS_SERVICE_ID_) OS_SERVICES };
/* The deadline owners of the image, one slot each (runtime/deadline.c): every task that can register one. The image's
 * own tables size the slots from this sum (tools/sunstone-manifest.py), since only they see the services that the image
 * links per application (the headset and the video decoder). The terms:
 * - each linked start entry's owners column, with the DMA0 pool's two tasks (dma-boot and dma-service) counted once in
 *   an image that links both the audio and the dma rows: platform/a733/dma_pool_session.c creates them for one pool
 *   session per boot (its instance, which a second initialization refuses);
 * - the board's four registering tasks (apps/board/main.c): board-usb2, board-radio, board-net and board-watchdog, the
 *   supervisor. board-pmic, board-usb0, board-typec and board-power register none;
 * - the application's main task, and its GPU peer task in an image with the GPU queue (apps/os/application.c);
 * - the large-region loader task in an image that links the loader's read-only SD0 binding (apps/os/large_sd.c,
 *   SUNSTONE_A733_OS_LARGE_SD, set by cmake/sunstone-image.cmake for the image's tables). In a write-capable image the
 *   loader reads through the SD0 owner (the sd row) and registers none;
 * - the background load's tasks (runtime/bg_load.c), a measurement option;
 * - the latency diagnostic's wake tasks in its precise mode (apps/latency/workload.c), a measurement option. */
#define OS_DMA_POOL_OWNERS 2u
#define OS_BOARD_DEADLINE_OWNERS 4u
#define OS_APPLICATION_DEADLINE_OWNERS (1u + OS_WITH_GPU(1u +) 0u)
#if defined(SUNSTONE_A733_OS_LARGE_SD) && !defined(SUNSTONE_A733_OS_SD_WRITE)
#define OS_LARGE_SD_DEADLINES 1u
#else
#define OS_LARGE_SD_DEADLINES 0u
#endif
#ifdef SUNSTONE_BG_LOAD
#include "sunstone_bg_count.h"
#define OS_BG_DEADLINES SUNSTONE_BG_TASKS
#else
#define OS_BG_DEADLINES 0u
#endif
/* The latency diagnostic's precise mode registers its eight wake tasks, one per core (apps/latency/workload.c,
 * latency_workload_init); checks/deadline-owners.py counts them from the wake task's TCB array. */
#define OS_LATENCY_PRECISE_OWNERS 8u
#ifdef SUNSTONE_LATENCY_PRECISE
#define OS_LATENCY_DEADLINES OS_LATENCY_PRECISE_OWNERS
#else
#define OS_LATENCY_DEADLINES 0u
#endif
#define OS_START_OWNERS_(name, prefix, start, with, owners) with((owners) +)
#define OS_DEADLINE_OWNERS (OS_START_ROWS(OS_START_OWNERS_) 0u - OS_WITH_DMA(OS_DMA_POOL_OWNERS +) 0u + \
    OS_BOARD_DEADLINE_OWNERS + OS_APPLICATION_DEADLINE_OWNERS + OS_LARGE_SD_DEADLINES + OS_BG_DEADLINES + \
    OS_LATENCY_DEADLINES)
/* The supervision table's rows after the application's, and the grant each one needs, indexed by service id. */
#define OS_SERVICE_ENTRY_(NAME, name, window_us, grant, with) with({name, window_us},)
#define OS_SERVICE_TABLE OS_SUPERVISED_ROWS(OS_SERVICE_ENTRY_)
#define OS_SERVICE_GRANT_(NAME, name, window_us, grant, with) with([OS_SERVICE_##NAME] = (grant),)
#define OS_SERVICE_GRANTS {[1] = OS_GRANT_USB0, OS_SUPERVISED_ROWS(OS_SERVICE_GRANT_)}
/* The start list's entries and their prototypes (struct os_application, apps/os/application.h). */
#define OS_START_PROTOTYPE_(name, prefix, start, with, owners) \
    int prefix##_task_privileged(void (*)(void *), void *); int prefix##_pins(struct a733_pin_handoff *); int start(void);
#define OS_START_PROTOTYPES OS_START_ROWS(OS_START_PROTOTYPE_)
#define OS_START_ENTRY_(name, prefix, start, with, owners) with({name, prefix##_task_privileged, prefix##_pins, start},)
#define OS_START_TABLE OS_START_ROWS(OS_START_ENTRY_)
#endif
