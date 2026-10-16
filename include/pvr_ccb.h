#ifndef SUNSTONE_PVR_CCB_H
#define SUNSTONE_PVR_CCB_H
#include <stddef.h>
#include <stdint.h>

#define PVR_CCB_CONTROL_BYTES 144u
#define PVR_CCB_COMMAND_BYTES 88u
#define PVR_CCB_HOST_SLOTS 128u
#define PVR_CCB_FIRMWARE_SLOTS 32u
#define PVR_CCB_MAGIC UINT32_C(0x2abc0000)
#define PVR_CCB_HEALTH (PVR_CCB_MAGIC | 115u)

enum pvr_ccb_region { PVR_CCB_HOST_CONTROL, PVR_CCB_HOST_COMMANDS,
    PVR_CCB_RETURNS, PVR_CCB_FIRMWARE_CONTROL, PVR_CCB_FIRMWARE_COMMANDS };
enum pvr_ccb_barrier { PVR_CCB_ACQUIRE, PVR_CCB_RELEASE, PVR_CCB_DEVICE_COMPLETE };
enum pvr_ccb_result { PVR_CCB_OK, PVR_CCB_AGAIN, PVR_CCB_INVALID,
    PVR_CCB_EXPIRED, PVR_CCB_FAULT, PVR_CCB_RESTART };
enum pvr_ccb_phase { PVR_CCB_NEW, PVR_CCB_PREPARED, PVR_CCB_ACTIVE, PVR_CCB_FAILED };

/* Operations return 1 on success and 0 on failure. They must be bounded,
 * non-reentrant, and preserve byte order. Four-byte control/return accesses
 * must use a single aligned little-endian word transaction. Bulk transfers
 * access shared RAM, never MMIO. ACQUIRE makes firmware writes visible before
 * subsequent reads; RELEASE orders prior reads and publishes prior host writes
 * before later writes; DEVICE_COMPLETE
 * completes the MMIO kick. Use uncached shared mappings or implement cache
 * maintenance without overwriting firmware-owned cache lines. A failed write
 * may have taken effect: the queue always retains ownership after failure.
 */
struct pvr_ccb_ops {
    int (*read)(void *, enum pvr_ccb_region, uint32_t, void *, size_t);
    int (*write)(void *, enum pvr_ccb_region, uint32_t, const void *, size_t);
    int (*barrier)(void *, enum pvr_ccb_barrier);
    int (*kick)(void *, uint32_t register_offset, uint32_t value);
};
struct pvr_ccb_token { uint64_t epoch, serial; uint32_t slot; };
struct pvr_ccb_slot { uint64_t serial, deadline; uint32_t state, status; };

/* Caller-owned state. Zero once before initialization. Fields are private to
 * this component except phase and last_event, which may be read for diagnosis.
 * Serialize every API call through one privileged FreeRTOS worker or lock.
 */
struct pvr_ccb {
    struct pvr_ccb_ops ops;
    void *context;
    uint64_t epoch, serial, last_now;
    uint32_t phase, host_write, host_read, firmware_read;
    struct pvr_ccb_slot slots[PVR_CCB_HOST_SLOTS];
    unsigned char last_event[PVR_CCB_COMMAND_BYTES];
};

/* Format stopped-device shared regions. Exact required sizes are returned here.
 * The platform owns allocation, mappings, physical/CPU alias checks and power.
 * A fresh nonzero epoch must distinguish every reset of this queue. On any I/O
 * failure, keep all shared buffers alive until an independently verified stop.
 * Reinitializing PREPARED/ACTIVE/FAILED state is refused. After a verified stop,
 * the caller may discard/zero state and initialize with a different epoch.
 */
size_t pvr_ccb_region_bytes(enum pvr_ccb_region region);
enum pvr_ccb_result pvr_ccb_init(struct pvr_ccb *, const struct pvr_ccb_ops *, void *, uint64_t epoch);
/* Call only after firmware boot and queue-address setup. Firmware may already
 * have produced events. Host commands must still be empty. */
enum pvr_ccb_result pvr_ccb_start(struct pvr_ccb *, uint64_t now);

/* Nonblocking transport for an already admitted, trusted 88-byte command.
 * Payload semantics, firmware pointers, resource lifetimes, and permissions
 * belong to the command producer. This is not an application admission API.
 * The command header must carry the firmware magic and a non-task command ID.
 * Deadline and now use one monotonic 64-bit clock that does not wrap in-session.
 * AGAIN means no slot was published; EXPIRED means the supplied deadline passed.
 * Token is written only after publication and the kick complete successfully.
 */
enum pvr_ccb_result pvr_ccb_submit(struct pvr_ccb *, const void *command,
                                  uint64_t now, uint64_t deadline, struct pvr_ccb_token *);
/* One bounded scan, at most 128 return-word reads. Service this on interrupts
 * and before pending deadlines. Timeout, corrupt metadata, or callback failure
 * latches FAILED; subsequent APIs perform no device access. No internal retries.
 */
enum pvr_ccb_result pvr_ccb_poll(struct pvr_ccb *, uint64_t now);
/* Collect the firmware's command-executed status, not GPU job completion.
 * Frees only the return-slot reservation. Resource release needs separate GPU
 * completion/fence evidence. Duplicate or stale tokens are refused. Collection
 * of an already recorded result remains available after failure, without I/O.
 */
enum pvr_ccb_result pvr_ccb_collect(struct pvr_ccb *, struct pvr_ccb_token, uint32_t *status);
/* Consume at most one 88-byte firmware event. Output changes only on OK or
 * RESTART. Restart/page-fault requests latch FAILED and return RESTART with the
 * event. Other valid event IDs are delivered for service-level dispatch.
 * last_event retains a copied event even if acknowledging its slot fails.
 */
enum pvr_ccb_result pvr_ccb_receive(struct pvr_ccb *, void *event);
/* Stop further queue I/O after an external device/service fault. No device
 * access or buffer release. Already recorded results remain collectable.
 * NEW is refused; PREPARED/ACTIVE/FAILED become or remain FAILED. */
enum pvr_ccb_result pvr_ccb_abort(struct pvr_ccb *);
#endif
