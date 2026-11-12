/* The OS services: the one declaration of each service that a session can grant, the record it carries, its entry in the
 * fixed start list and the tasks the supervisor watches. Everything else is derived from these rows:
 * - in C, the grant bits, whether the image links a service, the service ids and the supervision table (apps/os/os.h),
 *   the session decode of the records (apps/os/session.c), their wire size (apps/os/grants.c), the progress the
 *   supervisor requires (apps/os/services.c) and the start list (written by tools/sunstone-manifest.py from the macros);
 * - in Python, the record sizes, magics and versions of the host session builder (apps/os/host.py) and the grants and
 *   records that checks/os-session-grants.py covers, which read the rows of this file.
 * The numbers of the application ABI, the device bits and the session extent types, stay in include/sunstone_app.h and
 * include/sunstone_session.h; the rows name them. Adding a service is a row in each list that applies, plus its code:
 * the start entries, the record's struct in struct os_session and its validation in apps/os/admission.c.
 *
 * Python reads the rows as written: one X(...) per line, arguments separated by commas, no nested parentheses except in
 * a macro name the rows already use. This header includes nothing and declares no functions, so host models can include it
 * alone. */
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

/* X(name, prefix, start, with): an entry of the fixed start list, in list order, before the application's own
 * (apps/os/start.c). The entry's functions are prefix_task_privileged, prefix_pins and start. */
#define OS_START_ROWS(X) \
    X("thermal", os_thermal, os_thermal_start, OS_ALWAYS) \
    X("large", os_large, os_large_start, OS_ALWAYS) \
    X("tlbi", os_tlbi, os_tlbi_start, OS_WITH_TLBI) \
    X("npu", os_npu, os_npu_start, OS_ALWAYS) \
    X("audio", os_audio, os_capture_start, OS_ALWAYS) \
    X("dma", os_dma, os_dma_start, OS_WITH_DMA) \
    X("radio", os_radio, os_radio_start, OS_ALWAYS) \
    X("headset", os_headset, os_headset_start, OS_WITH_HEADSET) \
    X("gpu-queue", os_gpu_queue, os_gpu_queue_start_entry, OS_WITH_GPU)

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
/* The tasks that register a deadline owner (runtime/deadline.c): every service, the SD owner and the GPU peer. */
#define OS_DEADLINE_OWNERS (OS_SERVICES + 1u + OS_WITH_GPU(1u +) 0u)
/* The supervision table's rows after the application's, and the grant each one needs, indexed by service id. */
#define OS_SERVICE_ENTRY_(NAME, name, window_us, grant, with) with({name, window_us},)
#define OS_SERVICE_TABLE OS_SUPERVISED_ROWS(OS_SERVICE_ENTRY_)
#define OS_SERVICE_GRANT_(NAME, name, window_us, grant, with) with([OS_SERVICE_##NAME] = (grant),)
#define OS_SERVICE_GRANTS {[1] = OS_GRANT_USB0, OS_SUPERVISED_ROWS(OS_SERVICE_GRANT_)}
/* The start list's entries and their prototypes (struct os_application, apps/os/application.h). */
#define OS_START_PROTOTYPE_(name, prefix, start, with) \
    int prefix##_task_privileged(void (*)(void *), void *); int prefix##_pins(struct a733_pin_handoff *); int start(void);
#define OS_START_PROTOTYPES OS_START_ROWS(OS_START_PROTOTYPE_)
#define OS_START_ENTRY_(name, prefix, start, with) with({name, prefix##_task_privileged, prefix##_pins, start},)
#define OS_START_TABLE OS_START_ROWS(OS_START_ENTRY_)
#endif
