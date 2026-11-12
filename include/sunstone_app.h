#ifndef SUNSTONE_APP_H
#define SUNSTONE_APP_H
/* SunstoneOS application interface (docs/designs/app-abi.md).
 *
 * An application is code linked into the OS image together with one
 * manifest. The manifest is little-endian data in the section
 * "sunstone_manifest"; host tools and OS admission read it from the linked
 * image. tools/sunstone-manifest.py generates it from the application's
 * manifest.json, and checks/sunstone-app-abi.py compares the bytes that the
 * C compiler lays out with the bytes that the tool packs. */
#include <stddef.h>
#include <stdint.h>
#include "gpu_queue.h"
#include "sunstone_npu_queue.h"
#include "sunstone_cpu_topology.h"
#include "sunstone_cpu_observation.h"
#include "sunstone_large.h"
#include "sunstone_storage.h"
#ifdef __cplusplus
#define SS_STATIC_ASSERT static_assert
#define SS_ALIGNAS alignas
#define SS_NORETURN [[noreturn]]
extern "C" {
#else
#define SS_STATIC_ASSERT _Static_assert
#define SS_ALIGNAS _Alignas
#define SS_NORETURN _Noreturn
#endif

#define SS_MANIFEST_MAGIC UINT64_C(0x5453464e4d505041) /* "APPMNFST" */
#define SS_MANIFEST_VERSION 2u
/* Version 1 is a fixed record of SS_MANIFEST_V1_REGIONS region entries
 * (SS_MANIFEST_V1_BYTES). The OS still admits it; the region entries start at
 * the same offset, and region_count says how many are in use. */
#define SS_MANIFEST_VERSION_1 1u
#define SS_MANIFEST_V1_REGIONS 8u
#define SS_NAME_BYTES 16u
/* Bytes of an application manifest before its region table. The table holds
 * region_count entries, as many as the application declares; the section
 * bytes of one manifest are SS_MANIFEST_HEAD_BYTES plus region_count times
 * sizeof(struct ss_region). */
#define SS_MANIFEST_HEAD_BYTES 104u

/* Devices an application uses. USB0, the host channel, and the thermal
 * service are always present and have no bit. The NPU client is an OS service
 * that the image selects, not an application device. The radio station is in
 * every image; the session grants it with SS_DEVICE_RADIO and a radio
 * workload in the board prefix (1 with USB storage, 3 without). */
enum {
    SS_DEVICE_USB_STORAGE = 1u << 0,   /* USB2 host, mass-storage leaf */
    SS_DEVICE_USB_KEYBOARD = 1u << 1,  /* USB2 host, boot-protocol keyboard leaf */
    SS_DEVICE_AUDIO_I2S = 1u << 2,     /* I2S0 microphone capture */
    SS_DEVICE_AUDIO_HEADSET = 1u << 3, /* Bluetooth headset capture */
    SS_DEVICE_GPU = 1u << 4,
    SS_DEVICE_USB_MOUSE = 1u << 5,
    SS_DEVICE_USB_SERIAL = 1u << 6,
    SS_DEVICE_USB_CAMERA = 1u << 7,
    SS_DEVICE_RADIO = 1u << 8,         /* Wi-Fi station of the radio module */
    SS_DEVICE_USB_DEVICE = 1u << 9,    /* USB0 vendor device service */
    SS_DEVICE_ALL = (1u << 10) - 1
};
/* Preferred cluster of the main task. Without A76 admission the OS grants
 * the A55 pair 2-3 instead of the A76 pair 6-7. */
enum { SS_CLUSTER_A55 = 0, SS_CLUSTER_A76 = 1 };
enum { SS_REGION_INPUT = 1, SS_REGION_WORK = 2, SS_REGION_OUTPUT = 3 };
enum {
    SS_ACCESS_READ = 1u << 0,      /* CPU read at the application's level */
    SS_ACCESS_WRITE = 1u << 1,     /* CPU write */
    SS_ACCESS_GPU_READ = 1u << 2,
    SS_ACCESS_GPU_WRITE = 1u << 3,
    SS_ACCESS_SCRATCH = 1u << 4    /* work or output the board runner does not back up or restore; no OS code reads it */
};

/* One physical range; virtual address equals physical address. The build
 * assigns the address when manifest.json omits it. INPUT: bytes is the
 * maximum content length, and the exact length when sha256 is nonzero.
 * CPU access is a trusted application contract. Current mappings keep INPUT
 * regions writable; the existing GPU output profile enforces CPU read-only
 * access. INPUT authentication does not protect against later application writes.
 * WORK and OUTPUT: bytes is the capacity. sha256 is zero except for an INPUT
 * whose content the manifest pins; otherwise the session supplies it. */
struct ss_region {
    uint64_t address;
    uint64_t bytes;
    uint32_t kind;
    uint32_t access;
    uint8_t sha256[32];
};

/* Optional application registration in the immutable section
 * "sunstone_region_initializers", aligned to 8 bytes. Before CPU mappings
 * become read-only, EL3 clears and publishes this prefix of a GPU-written
 * WORK region. No record means no initialization. The prefix must be a
 * positive multiple of eight within the region; reserved must be zero. */
struct ss_region_initializer {
    uint32_t region, reserved;
    uint64_t zero_prefix_bytes;
};
SS_STATIC_ASSERT(sizeof(struct ss_region_initializer) == 16 &&
    offsetof(struct ss_region_initializer, zero_prefix_bytes) == 8, "region initializer layout");

/* Capture format; rate_hz zero means no capture. Samples are little-endian
 * signed integers, channels interleaved. Admission requires the session's
 * capture length to lie within min_frames..max_frames. */
struct ss_audio_format {
    uint32_t rate_hz;
    uint16_t channels, bits;
    uint32_t min_frames, max_frames;
};

/* clock_use: the application may be granted CPU operating-point changes, and, with them, the performance-mode service. */
enum { SS_CLOCK_GRANTS = 1, SS_CLOCK_PERF = 2 };
struct ss_manifest {
    uint64_t magic;
    uint32_t version, bytes;          /* bytes == SS_MANIFEST_HEAD_BYTES + region_count * sizeof(struct ss_region) */
    char name[SS_NAME_BYTES];         /* NUL-padded [a-z0-9-] */
    uint32_t devices_required;        /* SS_DEVICE_* the session must grant */
    uint32_t devices_optional;        /* SS_DEVICE_* the session may also grant */
    uint8_t cluster;                  /* SS_CLUSTER_* */
    uint8_t workers;                  /* worker tasks besides the main task */
    uint8_t priority;                 /* FreeRTOS priority of the main task, at EL0 */
    uint8_t reserved0;
    uint32_t stack_bytes;             /* main task stack */
    uint64_t heap_bytes;              /* added to the image's C/C++ heap */
    uint64_t run_deadline_us;         /* main start to ss_finish; 0: session bound */
    uint64_t return_reserve_us;       /* latest ss_finish to the session deadline */
    uint64_t progress_window_us;      /* longest gap between ss_progress calls */
    uint32_t output_limit;            /* maximum ss_finish bytes */
    /* Within 0x00..0x7f and outside the OS operations 0x05..0x08; the OS
     * also owns 0x80..0xff. */
    uint8_t host_op_first, host_op_count;
    uint8_t region_count;
    uint8_t clock_use;                /* SS_CLOCK_*: what the application may ask of the CPU clock service */
    struct ss_audio_format audio;
    struct ss_region regions[];       /* region_count entries, in manifest order */
};
/* Storage for a manifest of n regions, for generated sources, hand-written
 * manifests and tests: the union supplies the bytes that the flexible
 * array needs. Access it as .manifest. */
#define SS_MANIFEST_V1_BYTES (SS_MANIFEST_HEAD_BYTES + SS_MANIFEST_V1_REGIONS * 56u)
#define SS_MANIFEST_STORAGE(n) union { struct ss_manifest manifest; \
    SS_ALIGNAS(8) unsigned char bytes[SS_MANIFEST_HEAD_BYTES + (n) * 56u]; }

/* Nonzero when the version and size of a manifest in the section agree: version 2
 * is the head plus region_count entries, version 1 the fixed eight-entry record. */
static inline int ss_manifest_size_valid(const struct ss_manifest *m)
{
    if (m->version == SS_MANIFEST_VERSION)
        return m->bytes == SS_MANIFEST_HEAD_BYTES + (size_t)m->region_count * sizeof(struct ss_region);
    return m->version == SS_MANIFEST_VERSION_1 && m->bytes == SS_MANIFEST_V1_BYTES &&
           m->region_count <= SS_MANIFEST_V1_REGIONS;
}

SS_STATIC_ASSERT(sizeof(struct ss_region) == 56 && offsetof(struct ss_region, kind) == 16 &&
    offsetof(struct ss_region, sha256) == 24, "ss_region layout");
SS_STATIC_ASSERT(sizeof(struct ss_audio_format) == 16 && offsetof(struct ss_audio_format, min_frames) == 8 &&
    offsetof(struct ss_audio_format, max_frames) == 12, "ss_audio_format layout");
SS_STATIC_ASSERT(offsetof(struct ss_manifest, name) == 16 && offsetof(struct ss_manifest, devices_required) == 32 &&
    offsetof(struct ss_manifest, cluster) == 40 && offsetof(struct ss_manifest, stack_bytes) == 44 &&
    offsetof(struct ss_manifest, heap_bytes) == 48 && offsetof(struct ss_manifest, run_deadline_us) == 56 &&
    offsetof(struct ss_manifest, return_reserve_us) == 64 && offsetof(struct ss_manifest, progress_window_us) == 72 &&
    offsetof(struct ss_manifest, output_limit) == 80 && offsetof(struct ss_manifest, host_op_first) == 84 &&
    offsetof(struct ss_manifest, region_count) == 86 && offsetof(struct ss_manifest, audio) == 88 &&
    offsetof(struct ss_manifest, regions) == SS_MANIFEST_HEAD_BYTES && sizeof(struct ss_manifest) == SS_MANIFEST_HEAD_BYTES, "ss_manifest layout");

/* What the admitted session granted the application. Immutable once the
 * main task starts. */
struct ss_span {
    void *base;
    size_t bytes;   /* INPUT: content length; WORK, OUTPUT: capacity */
};
/* struct ss_grant.npu_core when the session grants no NPU queue. */
#define SS_NPU_CORE_NONE 0xffffffffu
struct ss_grant {
    uint32_t devices;               /* granted subset of required | optional */
    uint32_t main_core;
    uint32_t worker_cores;          /* one bit per granted worker core */
    uint32_t region_count;
    const struct ss_span *regions;  /* region_count spans in manifest order; the image sizes the table from the manifest */
    uint32_t capture_frames;        /* I2S: exact frames; headset: profile-derived maximum; 0 without capture */
    uint32_t npu_core;              /* the core whose task may use the NPU queue (SS_NPU_CORE_NONE without it): the main core on the
                                     * A55 cluster, else a worker core, on which the application creates its producer task */
    uint64_t finish_by_us;          /* counter microseconds; 0 when resident */
};

/* Optional fixed composition of two trusted GPU applications. The immutable
 * image defines ss_gpu_share to consent to one shared program and every GPU
 * region of the owner manifest. The peer manifest repeats those exact regions;
 * its task receives a GPU-only grant on one of the owner's worker cores.
 * Both applications can address the shared map. They must use disjoint writes
 * or synchronize access until each accepted job's record has been collected.
 * The owner controls create/open/close/recover and final session completion.
 * The peer entry returns after its work; the OS drains its owed record before
 * publishing completion. No descriptor means one application as before. */
struct ss_gpu_share {
    const struct ss_manifest *owner, *manifest;
    void (*main)(const struct ss_grant *);
    const struct gpu_queue_program *program;
    uint32_t shared_regions, reserved;
};
extern const struct ss_gpu_share ss_gpu_share;

/* Calls from application to OS. All are plain function calls into the image
 * and are safe at EL0 and EL1. They use mapped memory, atomics and OS
 * synchronization. Task notifications enter the RTOS through its existing
 * privilege-safe port. Device MMIO stays in the OS-owned EL1 tasks.
 * Applications are trusted code. Device calls enforce grants; they do not
 * isolate hostile application memory accesses. */
const struct ss_grant *ss_grant(void);
/* Large regions declared in the section "sunstone_large_regions" (sunstone_large.h). */
/* Base of large region index once it is ready, else NULL; *bytes receives its capacity. */
void *ss_large_base(unsigned index, size_t *bytes);
int ss_large_status(unsigned index, struct ss_large_status *out);
/* Block until the region is ready or failed or timeout_us passes: 1 ready, -1 failed, 0 timeout. */
int ss_large_wait(unsigned index, uint64_t timeout_us);
/* Every task that the application creates, its main task and the tasks they create in turn, starts in the
 * application's address space with no call: a new task takes the space of the task that creates it. This call
 * puts a task in the space when other code created the task, for example a task of the OS; call it before the
 * task first runs. It changes nothing for a task already in the space. */
void ss_large_attach(void *task);
/* 1 live with the granted devices ready, 0 not ready yet, -1 ended. */
int ss_live(void);
/* One atomic store: this application's supervision record. */
void ss_progress(void);
/* Read-only 24 MHz system counter; cached startup grants EL0 access. The
 * serialized read orders diagnostic timestamps around ordinary data loads. */
uint64_t ss_counter_ticks(void);
/* Load hint: 1 while the thermal policy is throttling or has failed. */
int ss_throttled(void);
/* Why ss_throttled reports a load hint, for failure records. cause bit 0:
 * the published level is not NORMAL; bit 1: the publication is stale (the
 * counter in microseconds is past deadline_us); bit 2: fewer than five
 * channels have been seen. passive and stale are the policy channel masks.
 * Temperatures are millidegrees Celsius in channel order CPUB, DDR, NPU, CPUL,
 * GPU. A diagnostic snapshot: fields are read without stopping the owner. */
struct ss_thermal_status {
    uint32_t cause, level, seen, passive, stale, sequence;
    uint64_t now_us, deadline_us, start_us;
    int32_t millicelsius[5], reserved;
};
void ss_thermal_status(struct ss_thermal_status *status);
/* The radio station of an image that selects it (SUNSTONE_A733_OS_RADIO_SCAN): the stage it
 * has reached and whether it is finished. Copies a snapshot; images without the station report
 * zero. stage is the enum a733_radio_stage of include/a733_radio_result.h; done is 1 once the
 * scan has completed, or the station has failed, so that the application can finish and the OS
 * can shut the module down and publish the full result. */
struct ss_radio_status {
    uint32_t stage, failed, done, reserved;
};
void ss_radio_status(struct ss_radio_status *status);
/* 1 once the CPU clock owner has finished the session grant's requests,
 * whatever their result, or when the image grants no CPU operating points. */
int ss_clock_ready(void);
/* Read-only CPU topology and boot observations. Queries copy bounded records;
 * compute loops can retain them. SS_TOPOLOGY_UNCALIBRATED never infers a
 * capacity ratio from clock rates. Work ranges are half-open and quantum-aligned
 * except for the final tail. No partial ranges are written on insufficient space. */
int ss_cpu_topology_info(struct ss_cpu_topology_info *out);
int ss_cpu_topology(struct ss_cpu_info *out, size_t capacity, size_t *count);
int ss_work_split(uint64_t units, uint64_t cpu_mask, uint32_t quantum,
                  struct ss_work_range *out, size_t capacity, size_t *count);
/* Explicitly use the calibration's maximum-capacity weights. This helper
 * does not observe current clocks; a caller comparing execution at that point
 * must verify the operating state across its workload. */
int ss_work_split_max(uint64_t units, uint64_t cpu_mask, uint32_t quantum,
                      struct ss_work_range *out, size_t capacity, size_t *count);
int ss_cpu_observations(unsigned core, struct ss_cpu_observation *out);
/* Mark the bounded application operation observed by thermal reports.
 * Both calls publish atomic counters; success is 0 or 1. */
void ss_activity_begin(void);
void ss_activity_end(int success);
/* Copy the admitted CPU operating-point description; 0 without a grant. */
int ss_clock_info(char *line,size_t bytes);
/* High-water byte count of the image C/C++ heap. */
size_t ss_heap_peak(void);
/* Capture grant->capture_frames frames (up to that bound for a headset
 * segment, which may end early) for sequence number `sequence` in the
 * manifest's format from the granted capture device; a later sequence
 * releases earlier buffers. 1: *pcm and *bytes describe the samples, and the
 * SS_CAPTURE_HEADROOM bytes before *pcm are the caller's to overwrite (for
 * a container header); 0: in progress; -1: refused or ended. */
#define SS_CAPTURE_HEADROOM 64u
int ss_capture(uint32_t sequence, void **pcm, size_t *bytes);
/* Select recorded raw PCM from an authenticated INPUT region, starting at its
 * byte offset (for example, after a WAV header). No capture device may be
 * granted. The manifest supplies the format and reserves output storage.
 * frames is the window size within min_frames..max_frames. A shorter final
 * window is delivered only if it still meets min_frames. paced is 0 or 1.
 * One application task opens replay once, then calls ss_capture with consecutive
 * sequences starting at 1. Results are copied into capture storage with writable
 * headroom. Repeating a completed sequence returns the same bytes. The next
 * sequence releases that output; earlier sequences are then refused.
 * Pacing starts at the first request and follows cumulative frame duration.
 * A late consumer catches up to that clock. No wait, device or sample conversion
 * occurs inside this call. 1 opened, 0 refused; ss_capture returns -1 at EOF. */
int ss_capture_replay_open(uint32_t region, uint64_t offset, uint32_t frames, uint32_t paced);
/* Host channel. A request carries an operation number inside the manifest's
 * range. The OS passes the next request only after the USB transfer of the
 * previous reply has completed, so each request also confirms delivery of
 * the previous reply. The OS sends nothing for a request until the
 * application replies. */
struct ss_host_request {
    uint8_t op, flags;
    uint16_t count;
    uint32_t offset;
    uint64_t argument;
};
SS_STATIC_ASSERT(sizeof(struct ss_host_request) == 16 && offsetof(struct ss_host_request, offset) == 4 &&
    offsetof(struct ss_host_request, argument) == 8, "ss_host_request is the 16-byte wire request");
#define SS_HOST_PAYLOAD_BYTES 1024u
struct ss_host_reply {
    uint32_t version;   /* frame version word */
    uint32_t state;     /* 0 pending, 1 ready, 2 failed */
    uint32_t total;     /* application-defined total length */
    uint32_t bytes;     /* payload bytes, at most SS_HOST_PAYLOAD_BYTES */
    uint8_t tag[16];    /* frame bytes 48..63 */
    const void *data;   /* copied before ss_host_send returns */
};
/* Waits up to wait_us for a request; the USB0 owner wakes the waiting task
 * with a task notification. 1: *request is new; 0: none arrived; -1: ended. */
int ss_host_recv(struct ss_host_request *request, uint32_t wait_us);
/* 1: queued as the reply to the last request; -1: no request waiting, or
 * bytes too large. */
int ss_host_send(const struct ss_host_reply *reply);
/* USB storage leaf: read one 512-byte sector. Repeat the same call until it
 * returns 1 (done) or -1 (failed); 0 means in progress. */
int ss_storage_read(uint64_t sector, uint8_t data[512]);
/* USB keyboard leaf: the next boot-protocol report. 1 copied, 0 none, -1 ended. */
struct ss_key_report {
    uint64_t attachment, sequence;
    uint8_t address, speed, interface, protocol, endpoint, subclass, mode, bytes;
    uint8_t data[8];
};
int ss_key_report(struct ss_key_report *report);
/* Model metadata. Names contain at most 63 UTF-8 bytes plus a terminating zero; the model of a graph registered at run time
 * (ss_npu_register_graph) has an empty name, and ss_npu_model_find reaches only named models. Byte sizes describe concatenated
 * logical tensors, excluding device padding. The OS image links no model: every index is the one ss_npu_register_graph
 * returned, and the counts and sizes come from the graph's own header. */
#define SS_NPU_MODEL_NAME_BYTES 64u
struct ss_npu_model {
    char name[SS_NPU_MODEL_NAME_BYTES];
    unsigned inputs,outputs;
    size_t input_bytes,output_bytes;
};
/* Discovery returns zero while unavailable. Find returns 1 on success, zero for an unknown name or invalid output. Info
 * returns 1 with the description, -1 for an unknown index, an invalid output or while another request of the mailbox is
 * outstanding, and 0 while pending: a model the OS holds (every model of an OS image) is described by the OS's NPU task from
 * its own plan, so call again with identical arguments until it answers. No allocation occurs. */
unsigned ss_npu_model_count(void);
int ss_npu_model_info(unsigned index,struct ss_npu_model *);
int ss_npu_model_find(const char *name,unsigned *index);
/* One admitted application's NPU catalogue. Model is an image catalogue index.
 * Call repeatedly with identical arguments until completion: 0 pending,
 * 1 output tensors copied, -2 refused, -1 unavailable or retained failure.
 * Inputs/outputs concatenate logical tensors in model order. The caller must
 * keep both buffers alive and untouched while pending. Only one request may
 * be outstanding; a mismatched call does not cancel that request. The service
 * task bounds hardware SUBMIT to timeout_us (1..1000000). Queue, OPEN,
 * LOAD and CLOSE use the runtime request waits and session deadline; this
 * argument is not an end-to-end API deadline.
 * A failed service permanently retains the request and buffers until session
 * teardown. Request publication wakes the owner through an EL0-safe RTOS
 * notification; completion is acquired through the mailbox atomics. */
/* Upper bound of one request: the NPU address space of 32 bits less the 0x10000000 base where a slot's regions start
 * (drivers/npu/a733_npu_runtime.c, place). The session's declared tensor_bytes bounds each graph more tightly at admission. */
#define SS_NPU_MAX_TENSOR_BYTES 0xf0000000u
int ss_npu_run(unsigned model,const void *input,size_t input_bytes,
               void *output,size_t output_bytes,uint64_t timeout_us);
/* ss_npu_run returns -3 when this job failed with an NPU MMU exception and the NPU was reset: the output is not
 * valid and the session remains usable. ss_npu_last_fault then describes the fault: the MMU status (+0x384), the
 * faulting device address (+0x380), the CRC-32 of a test image's canary buffer (zero elsewhere), the service's
 * progress marks and the wait window in milliseconds. It returns 1 once per fault and 0 when there is none. */
struct ss_npu_fault_info { uint32_t mmu_status, mmu_address, canary_crc, marks, window_ms, reserved; };
int ss_npu_last_fault(struct ss_npu_fault_info *);
/* Hand the NPU a compiled graph at run time, for example a file read into a region: no list names it and the OS image is not
 * rebuilt. Call repeatedly with identical arguments until it completes: 0 pending, 1 registered (*index is the model index
 * for ss_npu_run), -2 refused (not admissible, beyond the declared graph bounds or the declared graph count), -1
 * unavailable or retained failure. The OS copies the graph into memory of its own before it admits it and reads only that
 * copy afterwards: once this returns 1 the application may reuse or free its buffer. The session must declare graph_bytes
 * and tensor_bytes (npu section of the manifest). */
int ss_npu_register_graph(const void *graph,size_t bytes,unsigned *index);
/* Why the last ss_npu_register_graph was refused (-2); zero when it was not. A session that names its graph region
 * (npu.graph_region in the manifest) has its pool sized exactly from those graphs at admission and has no room for another:
 * its registration is refused with SS_NPU_REGISTER_SEALED, never by shrinking or sharing what the others hold. */
enum ss_npu_register_refusal {
    SS_NPU_REGISTER_NONE, SS_NPU_REGISTER_NOT_ADMISSIBLE, SS_NPU_REGISTER_TOO_LARGE, SS_NPU_REGISTER_FULL,
    SS_NPU_REGISTER_SEALED, SS_NPU_REGISTER_MEMORY
};
int ss_npu_register_refusal(void);
/* After the final run, repeat until 1 (slot closed), or a negative failure.
 * Successful CLOSE is terminal for this session. */
int ss_npu_close(void);
/* Optional diagnostic observer, registered once before scheduling. Callbacks
 * run on the serialized EL1 worker, never block and access ordinary memory
 * only. START follows a matching RUNNING job's hardware-busy sample; the
 * callback records counters A. CANDIDATE returns 1 after counters B advance;
 * CONFIRMED follows the same job's second hardware-busy sample. DONE precedes
 * reap and records actual ISR receipts. An observer cannot delay completion.
 * Kick only notifies that worker; it performs no device operation. */
enum { SS_NPU_OBSERVE_CANDIDATE=0,SS_NPU_OBSERVE_START=1,
       SS_NPU_OBSERVE_DONE=2,SS_NPU_OBSERVE_CONFIRMED=3 };
struct ss_npu_observation {
    uint64_t ticket,token,client,model,trigger_ticks,sample_ticks,idle,state,irq_ticks,irq_count;
};
int ss_npu_observe(int (*)(void *,unsigned,const struct ss_npu_observation *),void *);
void ss_npu_observer_kick(void);


/* USB class calls return 1 for a result, 0 while pending, -1 on refusal or
 * disconnect. Input events are copied. Serial and camera payloads stay in
 * OS-owned buffers until explicit release; applications must not modify them.
 * One application consumer may use each queue. */
enum { SS_INPUT_KEY = 1, SS_INPUT_POINTER = 2 };
struct ss_input_event {
    uint64_t attachment, sequence;
    uint32_t kind, usage;
    int32_t value, x, y, wheel;
    uint32_t buttons, modifiers;
};
int ss_input_event(struct ss_input_event *event);
/* Main application task: sleep until a class completion or wait_us expires.
 * Call the class operation again after waking; notifications may coalesce. */
void ss_usb_wait(uint32_t wait_us);
/* Main application task: wait to an absolute microsecond deadline in the
 * ss_counter_ticks()/24 epoch, clipped to the granted work finish. A deadline
 * at or before the current time returns at once without arming an alarm; a
 * later deadline wakes at that microsecond, not at a scheduler tick. Wake hints
 * may return early; recheck the operation after each return. An exhausted
 * granted work limit fails the application instead of creating a retry loop. */
void ss_wait_until(uint64_t absolute_us);
struct ss_usb_serial_buffer {
    uint64_t token, sequence;
    const uint8_t *data;
    size_t bytes;
};
int ss_usb_serial_acquire(struct ss_usb_serial_buffer *buffer);
int ss_usb_serial_release(uint64_t token);
/* The caller keeps data immutable until write_result returns 1 or -1.
 * A pending write retains ownership even after disconnect until OS drain. */
int ss_usb_serial_write(uint64_t sequence, const void *data, size_t bytes);
int ss_usb_serial_write_result(uint64_t sequence, size_t *actual);
struct ss_usb_serial_line {
    uint32_t baud;
    uint8_t bits, parity, stop, rtscts; /* CDC parity/stop encodings */
};
int ss_usb_serial_configure(uint64_t sequence, const struct ss_usb_serial_line *line);
int ss_usb_serial_config_result(uint64_t sequence);
struct ss_usb_frame_slice { const uint8_t *data; size_t bytes; };
enum { SS_USB_FRAME_MJPEG = 1, SS_USB_FRAME_YUYV = 2 };
struct ss_usb_camera_frame {
    uint64_t token, sequence;
    const struct ss_usb_frame_slice *slices;
    size_t slice_count, bytes;
    uint32_t width, height, format, reserved;
};
int ss_usb_camera_acquire(struct ss_usb_camera_frame *frame);
int ss_usb_camera_release(uint64_t token);
struct ss_usb_decoded_frame {
    const uint8_t *data; /* packed active planar Y, Cb, Cr rows (YU12) */
    size_t bytes;
    uint32_t width, height;
};
/* One MJPEG decode per image, through the optional OS JPEG hardware owner.
 * Retain token until this call returns 1 or -1. Output stays read-only and
 * retained for the image lifetime. 0 pending; -1 refused or failed. */
int ss_usb_camera_decode(uint64_t token, struct ss_usb_decoded_frame *frame);
/* Copy the result into the OS result record and end this application. In a
 * returning lifetime the OS then returns to the host; in the resident
 * lifetime the record stays readable over the host channel. Refused
 * results (too large, after finish_by_us) end the session as ss_fail does. */
SS_NORETURN void ss_finish(const void *result, size_t bytes);
/* End the session with an application-defined detail word. */
SS_NORETURN void ss_fail(uint32_t detail);
/* End the session with a measured operand. The low 12 detail bits identify
 * the check; bits 16..30 retain application context. Value is recorded
 * unchanged in the first-failure breadcrumb. */
SS_NORETURN void ss_fail_value(uint32_t detail,uint32_t value);

/* Provided by the application, named after the manifest name with '-'
 * replaced by '_'; tools/sunstone-manifest.py generates their prototypes in
 * <name>-app.h. <name>_init is optional and runs once before the scheduler
 * starts; <name>_main runs in the main task, at EL0, after the OS has
 * authenticated every INPUT region. Returning from <name>_main is a failure. */

#ifdef __cplusplus
}
#endif
#endif
