#ifndef SUNSTONE_DEADLINE_H
#define SUNSTONE_DEADLINE_H
#include <stdint.h>
/* Notification index zero carries wake hints, never completion records. The
 * caller must recheck its authoritative device predicate after every result,
 * including expiry: a completion can race the deadline or cancellation.
 * Register once in each owning task. Registered tasks must not be deleted or
 * have their handles reused. There is no allocation or registration hot path. */
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
