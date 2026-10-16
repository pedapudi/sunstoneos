#ifndef SUNSTONE_PVR_CCCB_H
#define SUNSTONE_PVR_CCCB_H
#include "pvr_ccb.h"

#define PVR_CCCB_CONTROL_BYTES 32u
#define PVR_CCCB_HEADER_BYTES 40u
#define PVR_CCCB_MAX_COMMANDS 8u
#define PVR_CCCB_MAX_CLEANUPS 3u
#define PVR_CCCB_MAX_BYTES 32768u
#define PVR_CCCB_CHUNK_BYTES 256u
#define PVR_CCCB_TASK UINT32_C(0x8000)

enum pvr_cccb_region { PVR_CCCB_CONTROL, PVR_CCCB_COMMANDS };
enum pvr_cccb_phase { PVR_CCCB_NEW, PVR_CCCB_FORMAT, PVR_CCCB_IDLE,
    PVR_CCCB_SPACE, PVR_CCCB_WRITE, PVR_CCCB_PUBLISH, PVR_CCCB_SUBMIT,
    PVR_CCCB_WAIT, PVR_CCCB_FAILED };
enum pvr_cccb_result { PVR_CCCB_OK, PVR_CCCB_AGAIN, PVR_CCCB_INVALID,
    PVR_CCCB_FAULT };
enum pvr_cccb_error { PVR_CCCB_NO_ERROR, PVR_CCCB_IO, PVR_CCCB_CLOCK,
    PVR_CCCB_TIMEOUT, PVR_CCCB_QUEUE, PVR_CCCB_CURSOR, PVR_CCCB_COMMAND };

/* Callbacks return exactly 1 on success. They are bounded and non-reentrant.
 * Four-byte reads use one aligned little-endian transaction. Shared RAM must
 * use uncached mappings or equivalent cache ownership/maintenance. ACQUIRE
 * observes firmware writes; RELEASE publishes all preceding host writes.
 * now uses the same monotonic microsecond clock as the kernel command queue.
 * A failed write may have taken effect. No callback releases backing memory.
 */
struct pvr_cccb_ops {
    int (*now)(void *, uint64_t *);
    int (*read)(void *, enum pvr_cccb_region, uint32_t, void *, size_t);
    int (*write)(void *, enum pvr_cccb_region, uint32_t, const void *, size_t);
    int (*barrier)(void *, enum pvr_ccb_barrier);
};
struct pvr_cccb_command {
    uint32_t type, bytes, external_reference, internal_reference;
    const void *payload;
};
/* Private state except phase, error, serial and status for diagnostics. Zero
 * once before init. Serialize this transport and its kernel queue through one
 * privileged worker. Caller owns all RAM, mapping, power and context leases.
 */
struct pvr_cccb {
    struct pvr_cccb_ops ops;
    void *context;
    struct pvr_ccb *queue;
    uint64_t epoch, deadline, last_now, serial;
    uint32_t phase, error, status, size, firmware_context;
    uint32_t write, read, cursor, count, index, payload_offset, writing_payload;
    uint32_t total;
    struct pvr_ccb_token token;
    struct pvr_cccb_command commands[PVR_CCCB_MAX_COMMANDS];
    unsigned char kick[PVR_CCB_COMMAND_BYTES];
};

/* No I/O. Queue must be PREPARED or ACTIVE. Ring size is a power of two from
 * 256 through 32768 bytes; firmware_context is a nonzero 8-aligned firmware
 * address. Poll formats the control record before its deadline. The owning
 * firmware context must remain unattached throughout formatting. Platform must
 * prove control/ring backing disjoint from each other and every private object.
 * Init refusal preserves state. A failed session must not be reinitialized.
 */
int pvr_cccb_init(struct pvr_cccb *, const struct pvr_cccb_ops *, void *context,
                  struct pvr_ccb *, uint32_t size, uint32_t firmware_context,
                  uint64_t deadline);

/* Admit one trusted command sequence without I/O, only while IDLE and the
 * kernel queue is ACTIVE. Context/VM/program storage must already be valid and
 * published. The first KICK supplies its firmware context address; this API does
 * not require a separate firmware attachment command.
 * Payload lengths must be multiples of eight; each command uses a 40-byte
 * header. Total headers/payloads plus one header must fit in half the ring.
 * Task IDs 201..211 and generic IDs 212..219 are accepted with their ABI magic
 * and task bit. Payload semantics, pointers, permissions and command order
 * belong to the producer; this is not an application admission interface.
 * Descriptors are copied; payloads must stay immutable, mapped and disjoint
 * from shared device buffers until IDLE or independently verified isolation.
 * Optional cleanup addresses identify firmware-owned cleanup records; keep
 * those records alive until separate job-completion and cleanup evidence.
 * One KICK is outstanding per ring. Deadline covers space, writes and KICK
 * acceptance. Failure aborts the shared kernel queue and retains all leases.
 */
int pvr_cccb_begin(struct pvr_cccb *, const struct pvr_cccb_command *, unsigned count,
                   const uint32_t *cleanup_addresses, unsigned cleanup_count,
                   uint64_t deadline);

/* One bounded step: format 32 bytes, sample ring space, write at most 256 bytes,
 * publish, submit one KICK, or collect its return. The owner must separately
 * service firmware control/events and pvr_ccb_poll while this operation waits.
 * Returning OK after a sequence means firmware accepted the KICK (status 1).
 * It does not establish GPU completion, data visibility or safe resource reuse.
 * IDLE and FAILED polls do not call callbacks. No allocation, reset or retry.
 */
enum pvr_cccb_result pvr_cccb_poll(struct pvr_cccb *);
enum pvr_cccb_result pvr_cccb_abort(struct pvr_cccb *);
#endif
