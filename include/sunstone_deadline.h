#ifndef SUNSTONE_DEADLINE_H
#define SUNSTONE_DEADLINE_H
#include <stdint.h>
/* Notification index zero carries wake hints, never completion records. The
 * caller must recheck its authoritative device predicate after every result,
 * including expiry: a completion can race the deadline or cancellation.
 * Register in each owning task before its first wait. A second call from the same task returns 1 with the slot it
 * already holds; a call from a task whose task-local pointer names a slot it does not own returns 0. Registered tasks
 * must not be deleted or have their handles reused. There is no allocation or registration hot path. */
/* Owner slots, one for each task that registers. In the OS image (SUNSTONE_DEADLINE_IMAGE_SLOTS) the image's own tables
 * hold them, sized by OS_DEADLINE_OWNERS (apps/os/os-services.h), because only the tables see the services that the image
 * links per application. A host-check build of the OS sources without that option sizes a static table from the same
 * count; probe and host-check builds that name no count get this small default, enough for their handful of tasks. */
#ifndef SUNSTONE_DEADLINE_SLOTS
#ifdef SUNSTONE_A733_OS
#include "os-services.h"
#define SUNSTONE_DEADLINE_SLOTS OS_DEADLINE_OWNERS
#else
#define SUNSTONE_DEADLINE_SLOTS 16u
#endif
#endif
/* The task-local pointer index that holds a registered task's slot (configNUM_THREAD_LOCAL_STORAGE_POINTERS). */
#define SUNSTONE_DEADLINE_TLS 0
enum sunstone_deadline_result {
    SUNSTONE_DEADLINE_ERROR=-1,
    SUNSTONE_DEADLINE_NOTIFIED=0,
    SUNSTONE_DEADLINE_EXPIRED=1
};
int sunstone_deadline_register(void);
int sunstone_deadline_wait_until(uint64_t absolute_us);
/* Immutable owner identity and a certificate of its latest admitted wait.
 * woken_us is set on timer fire or notification return. The supervisor may
 * grant only a bounded observation interval after that time. */
struct sunstone_deadline_record {
    uint64_t sequence,deadline_us,woken_us;
    unsigned armed,failed;
};
int sunstone_deadline_owner_record(void *owner,struct sunstone_deadline_record *);
void sunstone_deadline_owner_progress(void *owner);
/* Platform backend. Counter microseconds and CNTV compare share an epoch.
 * rearm only programs the comparator; irq wakes at most one expired owner.
 * program(0) disables the timer. kick requests CPU0 rearm after publication. */
uint64_t sunstone_deadline_now_us(void);
void sunstone_deadline_kick(void);
void sunstone_deadline_program(uint64_t absolute_us);
void sunstone_deadline_rearm(void);
void sunstone_deadline_irq(void);
#endif
