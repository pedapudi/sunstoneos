#include "dma_service_i2s.h"
int dma_service_i2s_profile_valid(const struct dma_service_i2s_profile *p)
{
    struct a733_i2s_plan plan;
    return p && a733_i2s_plan(&p->format,&plan) && p->operations.acquire &&
        p->operations.acquire_poll && p->operations.release && p->operations.now_us &&
        p->operations.read && p->operations.write;
}
int dma_service_i2s_profile_equal(const struct dma_service_i2s_profile *a,
                                const struct dma_service_i2s_profile *b)
{
    return a->cookie == b->cookie &&
        a->format.module_hz == b->format.module_hz && a->format.rate_hz == b->format.rate_hz &&
        a->format.mclk_hz == b->format.mclk_hz && a->format.container_bits == b->format.container_bits &&
        a->format.slot_bits == b->format.slot_bits && a->format.channels == b->format.channels &&
        a->format.first_slot == b->format.first_slot &&
        a->operations.acquire == b->operations.acquire && a->operations.acquire_poll == b->operations.acquire_poll &&
        a->operations.release == b->operations.release && a->operations.now_us == b->operations.now_us &&
        a->operations.read == b->operations.read && a->operations.write == b->operations.write;
}
static int deadline(const struct a733_i2s_operations *ops, void *cookie,
                    uint64_t duration, uint64_t *out)
{
    uint64_t now;
    if (!duration || duration > INT64_MAX || !ops->now_us(cookie,&now) ||
        now > UINT64_MAX-duration) return 0;
    *out=now+duration;
    return 1;
}
enum a733_i2s_status dma_service_i2s_open(struct a733_i2s_capture *c,
    const struct dma_service_i2s_profile *p, struct a733_dma_controller *dma,
    unsigned channel, uint64_t timeout)
{
    uint64_t until;
    if (!deadline(&p->operations,p->cookie,timeout,&until)) return A733_I2S_CONFLICT;
    return a733_i2s_open(c,&p->format,&p->operations,p->cookie,dma,channel,until);
}
enum a733_i2s_status dma_service_i2s_close(struct a733_i2s_capture *c, uint64_t timeout)
{
    uint64_t until;
    if (!deadline(&c->operations,c->cookie,timeout,&until)) return A733_I2S_RECOVERY;
    return a733_i2s_close(c,until);
}
