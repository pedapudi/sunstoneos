#ifndef SUNSTONE_SESSION_H
#define SUNSTONE_SESSION_H
/* SunstoneOS session, host frame and result layouts for the OS, the stage-2
 * loader and host tools (docs/designs/app-abi.md). Applications do not read
 * these records; they use include/sunstone_app.h. */
#include <stddef.h>
#include <stdint.h>
#ifndef SS_STATIC_ASSERT
#ifdef __cplusplus
#define SS_STATIC_ASSERT static_assert
#else
#define SS_STATIC_ASSERT _Static_assert
#endif
#endif

/* The session page: the 592-byte board prefix (struct a733_board_session),
 * this header, then `extents` typed extents, each padded to 8 bytes. The
 * stage-2 loader checks `bytes` and the prefix identity, stamps the prefix
 * epoch and binds the SHA-256 of the first `bytes` bytes into the contract;
 * it bounds every extent and requires zero persisted board and CPU-grant
 * epochs before watchdog arming. OS admission validates service policy. */
#define SS_SESSION_PREFIX_BYTES 592u
#define SS_SESSION_MAX_BYTES 4096u
#define SS_SESSION_MAGIC UINT64_C(0x324e4f4953534553) /* "SESSION2" */
#define SS_SESSION_VERSION 1u
/* The loader compares the four SID words' digest with the prefix
 * sid_evidence. OS admission requires this flag when the grant has no USB
 * storage, the recorded mode that today's loader binds to the board. */
#define SS_SESSION_BIND_SID 1u
struct ss_session_header {
    uint64_t magic, version;
    uint32_t bytes;                 /* complete session, prefix included */
    uint32_t extents;
    uint32_t devices;               /* granted SS_DEVICE_* bitmap */
    uint32_t flags;                 /* SS_SESSION_* */
    uint8_t manifests_sha256[32];   /* SHA-256 of the image's sunstone_manifest section */
};
struct ss_extent {
    uint32_t type;
    uint32_t bytes;                 /* payload bytes that follow */
};
/* OS service extents carry today's records unchanged: thermal facts, the I2S
 * receive facts (whose capture_samples is the capture length), the NPU
 * client facts and the CPU clock grant. Admission loads a copy of the grant
 * with the prefix epoch stamped in when the grant names clusters. */
enum {
    SS_EXTENT_THERMAL = 1, SS_EXTENT_AUDIO = 2, SS_EXTENT_NPU = 3, SS_EXTENT_CPU_GRANT = 4, SS_EXTENT_JPEG = 5, SS_EXTENT_USB_MEMORY = 6,
    SS_EXTENT_DMA = 7,             /* adopted DMA0 RAM-copy service facts */
    SS_EXTENT_INPUTS = 16           /* struct ss_input_fact per INPUT region, manifest order */
};
/* USB class profile's reserved NC pool and read-only application alias.
 * evidence identifies the reviewed reservation for this exact range under
 * the board prefix memory authority. Geometry alone is not physical ownership
 * evidence; the original boot-image reservation hash does not cover this pool. */
struct ss_usb_memory_extent {
    uint64_t version, base, bytes, alias;
    uint8_t evidence[32];
};
SS_STATIC_ASSERT(sizeof(struct ss_usb_memory_extent)==64,"USB memory extent layout");
struct ss_input_fact {
    uint64_t bytes;
    uint8_t sha256[32];
};
SS_STATIC_ASSERT(sizeof(struct ss_session_header) == 64 && offsetof(struct ss_session_header, devices) == 24 &&
    offsetof(struct ss_session_header, manifests_sha256) == 32, "ss_session_header layout");
SS_STATIC_ASSERT(sizeof(struct ss_extent) == 8 && sizeof(struct ss_input_fact) == 40, "ss_extent layout");

/* Host channel frame header: today's 64-byte USB0 response header. */
#define SS_FRAME_MAGIC UINT32_C(0x424d494d)
/* OS operations: 0x05..0x08 (fault, NPU, thermal and performance reports,
 * with today's numbers) and 0x80..0xff for new ones. */
#define SS_HOST_OS_REPORT_FIRST 0x05u
#define SS_HOST_OS_REPORT_LAST 0x08u
#define SS_HOST_OS_FIRST 0x80u
/* Paged read of the complete ss_result record, including its header. */
#define SS_HOST_RESULT_OP 0x80u
struct ss_host_frame {
    uint32_t magic, version, state, bytes, total, offset, storage, op;
    uint64_t epoch, generation;
    uint8_t tag[16];
};
SS_STATIC_ASSERT(sizeof(struct ss_host_frame) == 64 && offsetof(struct ss_host_frame, epoch) == 32 &&
    offsetof(struct ss_host_frame, tag) == 48, "ss_host_frame layout");

/* OS-owned result record that ss_finish writes; the payload follows. */
#define SS_RESULT_MAGIC UINT64_C(0x31544c5553455241) /* "ARESULT1" */
#define SS_RESULT_VERSION 1u
enum { SS_RESULT_FINISHED = 1 };
struct ss_result {
    uint64_t magic, version, bytes;  /* bytes: header plus payload capacity */
    uint64_t epoch, generation, status, payload_bytes;
};
SS_STATIC_ASSERT(sizeof(struct ss_result) == 56 && offsetof(struct ss_result, status) == 40 &&
    offsetof(struct ss_result, payload_bytes) == 48, "ss_result layout");
#endif
