/* Platform backend of runtime/deadline.c for the host shim: the microsecond clock, the
 * compare timer and the cross-core rearm request. Linked only with runtime/deadline.c. */
#include "host_rtos.h"
#include "sunstone_deadline.h"

static unsigned defer_kicks, kick_pending;
uint64_t sunstone_deadline_now_us(void) { return host_time_us(); }
void sunstone_deadline_program(uint64_t us) { host_timer_program(us); }
void host_deadline_defer_kicks(unsigned defer) { defer_kicks = defer; }
unsigned host_deadline_kick_pending(void) { return kick_pending; }
void sunstone_deadline_kick(void)
{
    if (defer_kicks) { kick_pending = 1; return; }
    sunstone_deadline_rearm();
}
void host_deadline_deliver_kick(void)
{
    kick_pending = 0;
    sunstone_deadline_rearm();
}
