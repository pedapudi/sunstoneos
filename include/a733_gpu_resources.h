#ifndef SUNSTONE_A733_GPU_RESOURCES_H
#define SUNSTONE_A733_GPU_RESOURCES_H
#include <stdint.h>
#include <stdatomic.h>
#include "a733_gpu_power.h"

#define A733_GPU_RESOURCE_CLIENTS 16u
#define A733_GPU_RESOURCE_GUARDS 16u
#define A733_GPU_RESOURCE_SLOTS (A733_GPU_RESOURCE_CLIENTS + A733_GPU_RESOURCE_GUARDS)
#define A733_GPU_RESOURCE_OSC       (1u << 0)
#define A733_GPU_RESOURCE_PARENT    (1u << 1)
#define A733_GPU_RESOURCE_RAIL      (1u << 2)
#define A733_GPU_RESOURCE_PCK       (1u << 3)
#define A733_GPU_RESOURCE_AHB       (1u << 4)
#define A733_GPU_RESOURCE_MBUS      (1u << 5)
#define A733_GPU_RESOURCE_LOCAL     (1u << 6)
#define A733_GPU_RESOURCE_ALL       127u

enum a733_gpu_resource_result { A733_GPU_RESOURCE_OK, A733_GPU_RESOURCE_INVALID,
    A733_GPU_RESOURCE_BUSY, A733_GPU_RESOURCE_FAULT };
enum a733_gpu_resource_error { A733_GPU_RESOURCE_NO_ERROR, A733_GPU_RESOURCE_IO,
    A733_GPU_RESOURCE_CLOCK, A733_GPU_RESOURCE_TIMEOUT, A733_GPU_RESOURCE_OSCILLATOR,
    A733_GPU_RESOURCE_PLL, A733_GPU_RESOURCE_SUPPLY, A733_GPU_RESOURCE_GATE,
    A733_GPU_RESOURCE_LIVE };
enum a733_gpu_resource_mode { A733_GPU_RESOURCE_FREE, A733_GPU_RESOURCE_STABLE,
    A733_GPU_RESOURCE_CHANGE, A733_GPU_RESOURCE_RETAINED };
struct a733_gpu_resource_rail { uint8_t page_before, chip, revision, enable, voltage, page_after; };
/* All callbacks are bounded and serialized by the manager. They return 1 for
 * success; failure can have effects. now uses monotonic microseconds. read32
 * performs one ordered access. update32 atomically ORs bits into a register
 * under the platform's shared register lock, requires all required bits already
 * set in the same critical section, completes the write, and preserves
 * unrelated bits. It receives the absolute operation deadline for lock waiting.
 * rail reads AXP8191 address 0x36, registers ff/0e/0f/10/15/ff in that order,
 * holding the common PMIC/I2C lock for the entire snapshot. No PMIC data writes.
 * Changes to shared resources require a CHANGE lease. A stable LOCAL lease
 * grants exclusive GPU-local clock/reset and CORE6 control, with TOP5 reached
 * only through the shared power-domain owner's row. All clock-register
 * writers must use the same platform register lock, including unrelated fields.
 */
struct a733_gpu_resource_ops {
    int (*now)(void *, uint64_t *);
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*update32)(void *, uintptr_t, uint32_t required, uint32_t set, uint64_t deadline);
    int (*rail)(void *, struct a733_gpu_resource_rail *, uint64_t deadline);
};
struct a733_gpu_resource_lease {
    uint64_t token;
    uint32_t mask;
    enum a733_gpu_resource_mode mode;
};
struct a733_gpu_rail_guard {
    struct a733_gpu_resource_lease lease;
    unsigned client;
};
struct a733_gpu_resource_manager {
    struct a733_gpu_resource_ops ops;
    void *context;
    atomic_uint busy;
    uint64_t next_token, last_now, deadline;
    struct a733_gpu_resource_lease leases[A733_GPU_RESOURCE_SLOTS];
    enum a733_gpu_resource_error error;
    uint32_t initialized, failed, stage, last_value;
    uint32_t rail_guards;
    struct a733_gpu_resource_rail rail;
};
/* Initialize zeroed storage once, before concurrent access. Context and callback
 * storage must remain stable; ops must not overlap manager. One manager controls
 * these resources globally. Client IDs are trusted privileged registrations.
 */
int a733_gpu_resources_init(struct a733_gpu_resource_manager *,
                           const struct a733_gpu_resource_ops *, void *);
/* Reserve requested resources, adding oscillator dependencies. LOCAL requires
 * ALL and is exclusive. Other stable leases may share matching resources.
 * Verify inherited clocks/rail and enable requested PCK/AHB/MBUS gates. The
 * parent must already be integer-mode 400 MHz and DCDC4 enabled at 800 mV.
 * Only LOCAL ownership permits enabling GPU bus gates; shared requests may
 * adopt them when already enabled. PCK can be enabled by a shared lease.
 * This function neither retunes PLLs nor programs voltage. On admission, token
 * is written before I/O. Failure retains that lease and disables manager I/O.
 * INVALID/BUSY do not modify outputs. ready is written only on success.
 * Output objects must be disjoint from each other and manager storage.
 * Calls are task-only; BUSY is a nonblocking refusal, suitable for queue retry.
 */
enum a733_gpu_resource_result a733_gpu_resources_acquire(
    struct a733_gpu_resource_manager *, unsigned client, uint32_t mask,
    uint64_t deadline, uint64_t *token, struct a733_gpu_power_resources *ready);
/* Reserve an exclusive change guard only when no other lease intersects the
 * requested mask. No hardware operation is performed. This authorizes no
 * particular voltage/PLL sequence; the caller must implement and qualify it.
 * Release with success=0 after any uncertain external mutation to retain all
 * leases and disable subsequent manager I/O.
 */
enum a733_gpu_resource_result a733_gpu_resources_change(
    struct a733_gpu_resource_manager *, unsigned client, uint32_t mask, uint64_t *token);
enum a733_gpu_resource_result a733_gpu_resources_change_end(
    struct a733_gpu_resource_manager *, unsigned client, uint64_t token, int success);
/* Release a stable lease. LOCAL release verifies external reset asserted,
 * local clocks off, GPU_TOP status zero and exact static CORE6 policy/status
 * zero before dropping ownership. The shared PCK clock stays held until then.
 * Shared gate enablement persists for the resource manager's OS lifetime,
 * preserving inherited gates, the critical AHB/MBUS gates, and the PCK provider.
 * No release disables a PLL/oscillator/rail, changes a PMIC register, or frees
 * GPU backing. Failed leases require independent recovery, not API retries.
 */
enum a733_gpu_resource_result a733_gpu_resources_release(
    struct a733_gpu_resource_manager *, unsigned client, uint64_t token, uint64_t deadline);
/* Read-only exact lease observation under the manager's nonblocking lock.
 * Requires a healthy manager and the specified live stable lease, including
 * client, token, mask and mode. BUSY leaves ownership unresolved. No clock
 * sample, hardware operation, state mutation or release occurs. */
enum a733_gpu_resource_result a733_gpu_resources_status(
    struct a733_gpu_resource_manager *, unsigned client, const struct a733_gpu_resource_lease *);
/* Metadata-only bridge for the common rail ledger. Allocate one free manager
 * bridge slot16–31 with RAIL STABLE or CHANGE ownership. Legacy client IDs
 * remain0–15; bridge admission cannot consume a caller's fixed client slot. Existing GPU acquisitions/change
 * guards conflict normally. No clock sample, I2C/MMIO, gate enable, voltage
 * proof or power readiness occurs. The ledger owns the private returned guard.
 * Status/end authenticate client, token, mask and mode plus bridge provenance.
 * Legacy release/change_end cannot release a bridge. end requires independently
 * established quiescence; it only removes metadata. Guard storage and manager
 * must be disjoint. All operations are nonblocking and preserve refusal output.
 */
enum a733_gpu_resource_result a733_gpu_rail_guard_acquire(
    struct a733_gpu_resource_manager *, int change, struct a733_gpu_rail_guard *);
enum a733_gpu_resource_result a733_gpu_rail_guard_status(
    struct a733_gpu_resource_manager *, const struct a733_gpu_rail_guard *);
enum a733_gpu_resource_result a733_gpu_rail_guard_end(
    struct a733_gpu_resource_manager *, const struct a733_gpu_rail_guard *);
#endif
