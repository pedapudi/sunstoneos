#include "storage_wait.h"

int storage_wait_run(const struct storage_wait_ops *w, uint64_t deadline_us)
{
    for (;;) {
        int r = w->poll(w->context);
        if (r) return r;
        if (!w->live(w->context)) return -1;
        uint32_t delay = 0;
        enum sunxi_mmc_wait kind = w->hint(w->context, &delay);
        if (kind == SUNXI_MMC_WAIT_NONE) continue;                  /* a register condition of a few microseconds */
        uint64_t now = w->now_us(w->context), until;
        if (now >= deadline_us) return -1;
        uint32_t recheck = w->recheck_us > STORAGE_WAIT_IRQ_RECHECK_US ? w->recheck_us : STORAGE_WAIT_IRQ_RECHECK_US;
        until = kind == SUNXI_MMC_WAIT_TIMER ? now + delay : now + recheck;
        if (until > deadline_us) until = deadline_us;
        w->sleep_until(w->context, until);
    }
}
