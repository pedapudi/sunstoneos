#ifndef OS_DEVICE_FAULT_H
#define OS_DEVICE_FAULT_H
/* The fault report of an OS-image device service whose owner task makes every device access (the video decoder, G2D
 * and the RTC). docs/designs/failure-return-audit.md, "A faulted device service closes before it reports", gives the
 * rule: a service whose operation failed after its first write enters FAULT and gets one close before the quiesce
 * reports; the quiesce reports DONE once the close returns the service to CLOSED, and a board fault otherwise.
 *
 * Two tasks share the words below:
 * - The owner task writes the fault fields, then stores phase FAULT, then, when no close can settle the fault or once
 *   a close has started, stores reported = 1 with release order. A close stores close_started, then CLOSING. The
 *   fault fields are never rewritten after reported is stored: a reported service stays FAULT, and a FAULT service
 *   refuses the two calls that clear them, an open (os_device_open_begin) and a G2D run (os_device_clear_fault).
 *   Each open therefore begins one fault lifetime, and a service judges a fault against that lifetime's first.
 * - The quiesce step runs on the supervisor's core while the owner task closes. It loads reported with acquire order
 *   and decides from that word and quiesced alone. Two loads of phase and close_started can fall on either side of
 *   the close's stores, so a step that read them could report while a close that will succeed is running, and the
 *   sticky report would end the session at the watchdog (site 0x742).
 * Every memory order is written out: the host models execute without reordering, so they cannot catch a missing one. */
#include "FreeRTOS.h"
#include "task.h"
#include "quiesce.h"
#include "sunstone_crumb.h"
#include <stdatomic.h>
#include <stdint.h>

/* The service phases. */
enum os_device_phase { OS_DEVICE_CLOSED, OS_DEVICE_OPENING, OS_DEVICE_OPEN, OS_DEVICE_CLOSING, OS_DEVICE_FAULT };

struct os_device_fault {
    atomic_uint phase;
    /* 1 once a close has started since the last open: a FAULT service gets one close. Only the owner task reads it. */
    atomic_uint close_started;
    /* 1 once the fault must be reported; see above. */
    atomic_uint reported;
    atomic_uint quiesce_wanted, quiesced;
    /* The first fault's step and value since the open; written only through the functions below. */
    uint32_t fault_step, fault_value;
    /* The board fault site the quiesce reports (0x9n1), and the owner task the quiesce wakes. */
    uint32_t site;
    TaskHandle_t *owner;
};

/* No fault recorded: the start of an open, and of a G2D run on an OPEN service. */
static inline void os_device_clear_fault(struct os_device_fault *f){f->fault_step=0;f->fault_value=0;}

/* The start of an open in the owner task: OPENING, a close not yet started and no fault. */
static inline void os_device_open_begin(struct os_device_fault *f)
{
    atomic_store_explicit(&f->phase, OS_DEVICE_OPENING, memory_order_seq_cst);
    atomic_store_explicit(&f->close_started, 0, memory_order_seq_cst);
    os_device_clear_fault(f);
}

/* A refusal in the owner task, made before any write: records the step and value and leaves the phase to the caller,
 * which returns the service to CLOSED (an open) or keeps it OPEN (a run). Returns -1. */
static inline int os_device_refuse(struct os_device_fault *f, unsigned step, uint32_t value)
{
    f->fault_step=step;f->fault_value=value;return -1;
}

/* A failure in the owner task: keeps the first fault, enters FAULT and, when a close has started or unsettled is set
 * (the service's condition under which no close can settle the fault), publishes the report. Returns -1. */
static inline int os_device_fail(struct os_device_fault *f, unsigned step, uint32_t value, unsigned unsettled)
{
    if(!f->fault_step){f->fault_step=step;f->fault_value=value;}
    atomic_store_explicit(&f->phase, OS_DEVICE_FAULT, memory_order_seq_cst);
    if(atomic_load_explicit(&f->close_started, memory_order_seq_cst) || unsettled)
        atomic_store_explicit(&f->reported, 1, memory_order_release);
    return -1;
}

/* The start of a close in the owner task: close_started, then CLOSING. Returns 1 when the service was FAULT. */
static inline unsigned os_device_close_begin(struct os_device_fault *f)
{
    const unsigned faulted = atomic_load_explicit(&f->phase, memory_order_seq_cst) == OS_DEVICE_FAULT;
    atomic_store_explicit(&f->close_started, 1, memory_order_seq_cst);
    atomic_store_explicit(&f->phase, OS_DEVICE_CLOSING, memory_order_seq_cst);
    return faulted;
}

/* An open service, or a FAULT one whose close has not started and that the service's close can settle. */
static inline int os_device_closable(struct os_device_fault *f, unsigned settleable)
{
    const unsigned p = atomic_load_explicit(&f->phase, memory_order_seq_cst);
    return p == OS_DEVICE_OPEN ||
        (p == OS_DEVICE_FAULT && !atomic_load_explicit(&f->close_started, memory_order_seq_cst) && settleable);
}

/* The quiesce step: FAULT with the site and step << 24 | value once reported, DONE once quiesced, otherwise asks the
 * owner task to close and returns PENDING. */
static inline enum os_quiesce_result os_device_quiesce_step(struct os_device_fault *f, struct os_quiesce *q)
{
    if(atomic_load_explicit(&f->reported, memory_order_acquire)) {
        (void)os_quiesce_fault(q, SUNSTONE_CRUMB_FAIL_RETURN | SUNSTONE_CRUMB_FAIL_BOARD << 12 | f->site,
            f->fault_step << 24 | (f->fault_value & 0xffffff), SUNSTONE_CRUMB_HERE());
        return OS_QUIESCE_FAULT;
    }
    if(atomic_load_explicit(&f->quiesced, memory_order_seq_cst))return OS_QUIESCE_DONE;
    atomic_store_explicit(&f->quiesce_wanted, 1, memory_order_seq_cst);
    if(f->owner && *f->owner)xTaskNotifyGive(*f->owner);
    return OS_QUIESCE_PENDING;
}
#endif
