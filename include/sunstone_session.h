#ifndef SUNSTONE_SESSION_H
#define SUNSTONE_SESSION_H
/* SunstoneOS session, host frame and result layouts for the OS, the stage-2
 * loader and host tools (docs/designs/app-abi.md). Applications do not read
 * these records; they use include/sunstone_app.h. Status: prototype. */
#include <stddef.h>
#include <stdint.h>

/* The session page: the 592-byte board prefix (struct a733_board_session),
 * this header, then `extents` typed extents, each padded to 8 bytes. The
 * stage-2 loader checks `bytes` and the prefix identity, stamps the prefix
 * epoch and binds the SHA-256 of the first `bytes` bytes into the contract;
 * it reads no extent. */
#define SS_SESSION_PREFIX_BYTES 592u
#define SS_SESSION_MAX_BYTES 4096u
#define SS_SESSION_MAGIC UINT64_C(0x324e4f4953534553) /* "SESSION2" */
#define SS_SESSION_VERSION 1u
/* The loader compares the four SID words' digest with the prefix sid_evidence. */
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
    uint16_t type;
    uint16_t index;                 /* application index for per-application types */
    uint32_t bytes;                 /* payload bytes that follow */
};
/* OS service extents carry today's records unchanged: thermal facts, the I2S
 * receive facts, the NPU client facts and the CPU clock grant (whose zero
 * epoch means the prefix epoch). */
enum {
    SS_EXTENT_THERMAL = 1, SS_EXTENT_AUDIO = 2, SS_EXTENT_NPU = 3, SS_EXTENT_CPU_GRANT = 4,
    SS_EXTENT_INPUTS = 16,          /* struct ss_input_fact per INPUT region, manifest order */
    SS_EXTENT_PARAMS = 17           /* uint8_t sha256[32], then the opaque block */
};
struct ss_input_fact {
    uint64_t bytes;
    uint8_t sha256[32];
};
_Static_assert(sizeof(struct ss_session_header) == 64 && offsetof(struct ss_session_header, devices) == 24 &&
    offsetof(struct ss_session_header, manifests_sha256) == 32, "ss_session_header layout");
_Static_assert(sizeof(struct ss_extent) == 8 && sizeof(struct ss_input_fact) == 40, "ss_extent layout");

/* Host channel frame header: today's 64-byte USB0 response header. */
#define SS_FRAME_MAGIC UINT32_C(0x424d494d)
#define SS_HOST_OS_FIRST 0x80u        /* OS operations; applications use 0x00..0x7f */
struct ss_host_frame {
    uint32_t magic, version, state, bytes, total, offset, storage, op;
    uint64_t epoch, generation;
    uint8_t tag[16];
};
_Static_assert(sizeof(struct ss_host_frame) == 64 && offsetof(struct ss_host_frame, epoch) == 32 &&
    offsetof(struct ss_host_frame, tag) == 48, "ss_host_frame layout");

/* OS-owned result record that ss_finish writes; the payload follows. */
#define SS_RESULT_MAGIC UINT64_C(0x31544c5553455241) /* "ARESULT1" */
#define SS_RESULT_VERSION 1u
enum { SS_RESULT_FINISHED = 1 };
struct ss_result {
    uint64_t magic, version, bytes;  /* bytes: header plus payload capacity */
    uint64_t epoch, generation;
    uint32_t application, status;
    uint64_t payload_bytes;
};
_Static_assert(sizeof(struct ss_result) == 56 && offsetof(struct ss_result, application) == 40 &&
    offsetof(struct ss_result, payload_bytes) == 48, "ss_result layout");
#endif
