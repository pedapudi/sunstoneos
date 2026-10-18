/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_MAINTENANCE_WATCHDOG_H
#define SUNSTONE_A733_MAINTENANCE_WATCHDOG_H
#include <stdint.h>

struct a733_storage_grant;
struct a733_storage_update;
struct a733_storage_native_transport;
struct a733_counter_clock;
struct a733_platform_locks;
struct a733_maintenance_lease;

#define A733_MAINTENANCE_TRANSFER_MAGIC UINT64_C(0x53554e5744544c31)
#define A733_MAINTENANCE_OPERATION_US UINT64_C(1000000)

/* Privileged bootstrap publishes this immutable transfer once, after arming
 * CPUX WDT0 and validating physical recovery evidence for this image/session.
 * Hashes identify evidence; they cannot establish physical qualification.
 * Existing diagnostic launch contracts cannot supply this transfer.
 * All times use the shared 24 MHz physical counter converted to absolute us.
 * The storage grant is the single session/range/nonce authority. Its deadline
 * is the last permitted worker activity; reset_by_us additionally bounds the
 * qualified worst-case reset/ROM-return interval after the final permitted feed.
 * Admission reserves guard_us before the storage cutoff for reload completion.
 * Source/redirect ownership is exclusive until reset. No other image component
 * may write WDT0, RTC source selection or CPU0 ROM redirect registers.
 * The dedicated runtime must use the storage deadline, not a diagnostic exit.
 */
struct a733_maintenance_transfer {
    uint64_t magic,epoch,generation,lease;
    const struct a733_storage_grant *storage;
    uint64_t reload_before_us,reload_after_us;
    uint64_t minimum_reload_us,maximum_recovery_us,reset_by_us;
    uint64_t guard_us;
    uint8_t image[32],qualification[32];
    uint32_t rtc_control,rtc_divider,rtc_source;
};

/* One permanent native owner, never released even after refusal or failure.
 * Obtaining this address conveys no permission to feed or to access storage.
 */
struct a733_maintenance_lease *a733_maintenance_native_lease(void);

/* Before scheduler start, after task handles and transport identities exist.
 * Bind the actual permanent objects; service initialization occurs later in
 * its owning storage task. Transfer and referenced grant remain immutable.
 * The common counter must already be initialized. Native admission verifies
 * EL1, Device identity mappings and armed mode/config/output/source/redirect.
 * This function never arms or feeds the watchdog. Return 1 only on admission.
 */
int a733_maintenance_bind(struct a733_maintenance_lease *,
    const struct a733_maintenance_transfer *,struct a733_counter_clock *,
    struct a733_platform_locks *,struct a733_storage_native_transport *,
    struct a733_storage_update *,void *storage_task,void *usb_task);

/* Any task may check the live bound or permanently revoke it. A failed check
 * revokes feed; it does not stop DMA, cancel writes, release resources or prove
 * recovery. Existing in-flight register access must finish or reset externally.
 */
int a733_maintenance_alive(struct a733_maintenance_lease *);
void a733_maintenance_revoke(struct a733_maintenance_lease *);

/* Storage task only, after service initialization and each bounded worker
 * step, including the step that publishes its copied response. Reads the real
 * service, validates immutable grant/deadlines and publishes actual verified
 * BACKUP/WRITE completion counters. No caller-supplied progress value exists.
 * A response need not remain response_ready after the channel copies it.
 * The storage task must call this before starting another channel request.
 */
int a733_maintenance_storage_observe(struct a733_maintenance_lease *,
    const struct a733_storage_update *);

/* USB task only, after publishing actual transport callbacks. One bounded
 * observation and at most one reload. Renewal requires a new verified chunk,
 * its completed IN response and the immediately following valid OUT request.
 * INFO, startup, PMIC polls and repeated observations never authorize reload.
 * No reload may extend past the grant cutoff or qualified reset_by bound.
 * FINISH permanently disables reload while permitting its final response to
 * drain within the already-earned bound. Returns 1 while live (whether or not
 * reloaded), 0 after permanent revocation or expiry.
 * Register mutex waits are deadline-bounded; a bus stall relies on independently
 * qualified hardware expiry, never on the CPU clock callback.
 */
int a733_maintenance_poll(struct a733_maintenance_lease *);
#endif
