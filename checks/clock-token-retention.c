/* A device-local fault retains one clock token and leaves the registry and every other token alone (a733_clock_retain_token); a fault of the
 * registry itself, a733_clock_retain, still fails all of it. */
#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>

static int now(void *cookie, uint64_t *out) { (void)cookie; *out = 1; return 1; }
int main(void)
{
    struct a733_clock_leases r = {0};
    struct a733_clock_token sd, ths, other, next;
    assert(a733_clock_leases_init(&r, now, NULL));
    assert(a733_clock_acquire(&r, 10, 0, A733_CLOCK_SDC0, 100, &sd) == A733_CLOCK_OK);       /* the SD0 binding's token */
    assert(a733_clock_acquire(&r, 11, 0, A733_CLOCK_THS, 100, &ths) == A733_CLOCK_OK);       /* the thermal service's token */
    /* The SD0 token faults. */
    assert(a733_clock_retain_token(&r, &sd) == A733_CLOCK_OK && a733_clock_retain_token(&r, &sd) == A733_CLOCK_OK);
    assert(!atomic_load(&r.failed));
    assert(a733_clock_token_status(&r, &sd) == A733_CLOCK_FAULT);
    assert(a733_clock_token_status(&r, &ths) == A733_CLOCK_OK);                              /* thermal's token still works */
    assert(a733_clock_release(&r, &sd, 100) == A733_CLOCK_FAULT);                            /* held forever */
    assert(a733_clock_acquire(&r, 12, 0, A733_CLOCK_SDC0, 100, &other) == A733_CLOCK_BUSY);  /* its scope stays held */
    assert(a733_clock_acquire(&r, 12, 0, A733_CLOCK_I2S0, 100, &other) == A733_CLOCK_OK);    /* the rest of the registry works */
    assert(a733_clock_release(&r, &other, 100) == A733_CLOCK_OK);
    assert(a733_clock_release(&r, &ths, 100) == A733_CLOCK_OK);                              /* and thermal can release its own */
    assert(a733_clock_acquire(&r, 11, 0, A733_CLOCK_THS, 100, &next) == A733_CLOCK_OK);
    struct a733_clock_token forged = {.id = 77, .owner = 1};
    assert(a733_clock_retain_token(&r, &forged) == A733_CLOCK_INVALID);
    /* A registry-level retention still fails everything. */
    assert(a733_clock_retain(&r, &next) == A733_CLOCK_FAULT && atomic_load(&r.failed));
    assert(a733_clock_token_status(&r, &next) == A733_CLOCK_FAULT);
    assert(a733_clock_retain_token(&r, &next) == A733_CLOCK_FAULT);
    puts("PASS: clock token retention is local; registry retention is global");
    return 0;
}
