/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_SCHEDULER_NOTIFICATION_H
#define SS_SCHEDULER_NOTIFICATION_H
#include "owner.h"
#define SS_NOTIFICATION_VALUE UINT64_C(0xffffffff)
#define SS_NOTIFICATION_STATE (UINT64_C(3) << 32)
#define SS_NOTIFICATION_COMPLETED (UINT64_C(1) << 34)
#define SS_NOTIFICATION_GATE_UNIT (UINT64_C(1) << 35)
#define SS_NOTIFICATION_GATE (UINT64_C(127) << 35)
#define SS_NOTIFICATION_PAYLOAD ((UINT64_C(1) << 35) - 1)
enum ss_notification_state { SS_NOTIFY_NOT_WAITING, SS_NOTIFY_WAITING, SS_NOTIFY_RECEIVED };
enum ss_notification_action {
    SS_NOTIFY_INCREMENT, SS_NOTIFY_SET_BITS, SS_NOTIFY_OVERWRITE,
    SS_NOTIFY_NO_OVERWRITE, SS_NOTIFY_NO_ACTION, SS_NOTIFY_CLEAR_STATE,
    SS_NOTIFY_CLEAR_BITS, SS_NOTIFY_QUERY
};
struct ss_notification {
    _Alignas(8) _Atomic uint64_t word;
    _Atomic uint64_t next_ticket, serving;
};
struct ss_notification_result {
    uint32_t previous;
    enum ss_notification_state previous_state;
    bool accepted, completed_wait, used_gate;
};
struct ss_notification_guard {
    struct ss_notification *slot;
    const struct ss_owner_backend *backend;
    uint64_t ticket, payload;
};
void ss_notification_init(struct ss_notification *, uint32_t);
/* Caller holds a descriptor pin and masks local API IRQs until publication and
 * guard release finish. Exactly one full-word CAS is attempted on the fast
 * path; conflicts join the registered FIFO. No nested admission on one CPU.
 * completed_wait requests owner inspection; it never authorizes remote links. */
struct ss_notification_result ss_notification_apply(struct ss_notification *,
    enum ss_notification_action, uint32_t, const struct ss_owner_backend *);
/* Every compound wait/timeout and stable observer uses this gate. Register
 * before taking a ticket. Only payload may change while held; reserved bits
 * stay zero. Commit owner intent/hints before end; end releases count then
 * ticket and signals SEV. No intermediate payload may be observed outside it. */
void ss_notification_gate_begin(struct ss_notification *,
    const struct ss_owner_backend *, struct ss_notification_guard *);
void ss_notification_gate_end(struct ss_notification_guard *);
enum ss_notification_state ss_notification_payload_state(uint64_t);

/* Owner-owned full generation record; never read or written by a producer.
 * The caller keeps the record alive through timeout and publisher retirement. */
enum ss_notification_wait_kind { SS_NOTIFY_WAIT_STATE, SS_NOTIFY_WAIT_COUNT };
struct ss_notification_wait {
    uint64_t generation;
    enum ss_notification_wait_kind kind;
    bool active, blocked;
};
/* These helpers require an already-held guard. The owner commits its task
 * state/deadline before releasing it. Enter returns true when blocking is
 * required. Observe consumes only scheduler completion, retaining the latch.
 * Finish preserves the upstream final value/state check independently of
 * irrevocable scheduler readiness. A false matching flag leaves state intact.
 * The timeout argument is terminal expiry: pass true after a real timeout OR
 * when returning failure/zero from a zero-time API poll. This closes the active
 * owner wait even if no notification condition or completion latch exists.
 * Passing false with neither condition retains the active wait for a later
 * observation; it must not be used immediately before returning to the caller. */
bool ss_notification_wait_enter(struct ss_notification_guard *,
    struct ss_notification_wait *, uint64_t generation,
    enum ss_notification_wait_kind, uint32_t clear_entry, bool may_block);
bool ss_notification_wait_observe(struct ss_notification_guard *,
    struct ss_notification_wait *, uint64_t generation);
struct ss_notification_finish {
    uint32_t value;
    bool matched, condition, scheduler_completed;
};
struct ss_notification_finish ss_notification_wait_finish(
    struct ss_notification_guard *, struct ss_notification_wait *,
    uint64_t generation, uint32_t clear_exit, bool decrement, bool timeout);
#endif
