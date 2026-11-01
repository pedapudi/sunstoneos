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
#ifdef __cplusplus
#define SS_STATIC_ASSERT static_assert
#define SS_NORETURN [[noreturn]]
extern "C" {
#else
#define SS_STATIC_ASSERT _Static_assert
#define SS_NORETURN _Noreturn
#endif

#define SS_MANIFEST_MAGIC UINT64_C(0x5453464e4d505041) /* "APPMNFST" */
#define SS_MANIFEST_VERSION 1u
#define SS_NAME_BYTES 16u
#define SS_REGIONS 8u

/* Devices an application uses. USB0, the host channel, and the thermal
 * service are always present and have no bit. The radio station and the NPU
 * client are OS services that the image selects, not application devices. */
enum {
    SS_DEVICE_USB_STORAGE = 1u << 0,   /* USB2 host, mass-storage leaf */
    SS_DEVICE_USB_KEYBOARD = 1u << 1,  /* USB2 host, boot-protocol keyboard leaf */
    SS_DEVICE_AUDIO_I2S = 1u << 2,     /* I2S0 microphone capture */
    SS_DEVICE_AUDIO_HEADSET = 1u << 3, /* Bluetooth headset capture */
    SS_DEVICE_GPU = 1u << 4,
    SS_DEVICE_USB_MOUSE = 1u << 5,
    SS_DEVICE_USB_SERIAL = 1u << 6,
    SS_DEVICE_USB_CAMERA = 1u << 7,
    SS_DEVICE_ALL = (1u << 8) - 1
};
/* Preferred cluster of the main task. Without A76 admission the OS grants
 * the A55 pair 2-3 instead of the A76 pair 6-7. */
enum { SS_CLUSTER_A55 = 0, SS_CLUSTER_A76 = 1 };
enum { SS_REGION_INPUT = 1, SS_REGION_WORK = 2, SS_REGION_OUTPUT = 3 };
enum {
    SS_ACCESS_READ = 1u << 0,      /* CPU read at the application's level */
    SS_ACCESS_WRITE = 1u << 1,     /* CPU write */
    SS_ACCESS_GPU_READ = 1u << 2,
    SS_ACCESS_GPU_WRITE = 1u << 3
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

struct ss_manifest {
    uint64_t magic;
    uint32_t version, bytes;          /* bytes == sizeof(struct ss_manifest) */
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
    uint8_t reserved1;
    struct ss_audio_format audio;
    struct ss_region regions[SS_REGIONS];
};

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
    offsetof(struct ss_manifest, regions) == 104 && sizeof(struct ss_manifest) == 552, "ss_manifest layout");

/* What the admitted session granted the application. Immutable once the
 * main task starts. */
struct ss_span {
    void *base;
    size_t bytes;   /* INPUT: content length; WORK, OUTPUT: capacity */
};
struct ss_grant {
    uint32_t devices;               /* granted subset of required | optional */
    uint32_t main_core;
    uint32_t worker_cores;          /* one bit per granted worker core */
    uint32_t region_count;
    struct ss_span regions[SS_REGIONS]; /* manifest order */
    uint32_t capture_frames;        /* I2S: exact frames; headset: profile-derived maximum; 0 without capture */
    uint32_t reserved;
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
 * and are safe at EL0 and EL1: they use atomics and memory that EL0 maps, and
 * never MMIO, locks shared with interrupt handlers or system registers.
 * Device work happens in OS-owned EL1 tasks that these calls signal.
 * Applications are trusted code. Device calls enforce grants; they do not
 * isolate hostile application memory accesses. */
const struct ss_grant *ss_grant(void);
/* 1 live with the granted devices ready, 0 not ready yet, -1 ended. */
int ss_live(void);
/* One atomic store: this application's supervision record. */
void ss_progress(void);
/* Load hint: 1 while the thermal policy is throttling or has failed. */
int ss_throttled(void);
/* 1 once the CPU clock owner has finished the session grant's requests,
 * whatever their result, or when the image grants no CPU operating points. */
int ss_clock_ready(void);
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

/* Provided by the application, named after the manifest name with '-'
 * replaced by '_'; tools/sunstone-manifest.py generates their prototypes in
 * <name>-app.h. <name>_init is optional and runs once before the scheduler
 * starts; <name>_main runs in the main task, at EL0, after the OS has
 * authenticated every INPUT region. Returning from <name>_main is a failure. */

#ifdef __cplusplus
}
#endif
#endif
