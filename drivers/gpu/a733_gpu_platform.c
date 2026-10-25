#include "a733_gpu_platform.h"
#include "a733_power_domains.h"
#include "a733_range.h"
#include "a733_gpu_resources_native.h"
#include <stddef.h>

static int fail(struct a733_gpu_platform *p)
{
    if (p->power.phase == A733_GPU_POWER_STARTING || p->power.phase == A733_GPU_POWER_STOPPING)
        (void)a733_gpu_power_cancel(&p->power);
    p->phase = A733_GPU_PLATFORM_FAILED;
    return -1;
}
static int clock_ok(struct a733_gpu_platform *p)
{
    uint64_t now;
    if (!p->ops.now(p->context, &now) || now < p->last_now || now < p->power.last_now || now >= p->deadline)
        return fail(p), 0;
    p->last_now = now;
    return 1;
}
static int clock_read(void *context, uint64_t *out)
{ struct a733_gpu_platform *p = context; return p->ops.now(p->context, out); }
static int acquire(void *context, struct a733_gpu_power_resources *out)
{
    struct a733_gpu_platform *p = context;
    if (!p->token || p->delegated || p->phase != A733_GPU_PLATFORM_STARTING) return 0;
    *out = p->ready; p->delegated = 1;
    return 1;
}
static int release(void *context)
{
    struct a733_gpu_platform *p = context;
    if (!p->token || !p->delegated || p->phase != A733_GPU_PLATFORM_STOPPING) return 0;
    /* The parent platform retains the actual lease while the controller returns
     * its delegated handle. Actual manager release is a separate polling step. */
    p->delegated = 0;
    return 1;
}
static int read_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off, uint32_t *out)
{
    struct a733_gpu_platform *p = context;
    return p->token && p->delegated && p->ops.read(p->context, bank, off, out);
}
static int update_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off,
                       uint32_t clear, uint32_t set)
{
    struct a733_gpu_platform *p = context;
    return p->token && p->delegated &&
        p->ops.update(p->context, bank, off, clear, set, p->deadline);
}
int a733_gpu_platform_init(struct a733_gpu_platform *p, struct a733_gpu_resource_manager *m,
    unsigned client, const struct a733_gpu_platform_ops *ops, void *context)
{
    if (!p || !m || !ops || p->initialized || !m->initialized || client >= A733_GPU_RESOURCE_CLIENTS ||
        !ops->now || !ops->read || !ops->update ||
        !a733_extents_disjoint_checked((uintptr_t)p, sizeof(*p), (uintptr_t)m, sizeof(*m)) ||
        !a733_extents_disjoint_checked((uintptr_t)p, sizeof(*p), (uintptr_t)ops, sizeof(*ops))) return 0;
    *p = (struct a733_gpu_platform){.manager = m, .ops = *ops, .context = context,
                                   .client = client, .initialized = 1};
    const struct a733_gpu_power_ops power_ops = {clock_read, acquire, release, read_reg, update_reg};
    return a733_gpu_power_init(&p->power, &power_ops, p);
}
int a733_gpu_platform_begin(void *context, uint64_t deadline)
{
    struct a733_gpu_platform *p = context;
    if (!p || !p->initialized || (p->phase != A733_GPU_PLATFORM_NEW && p->phase != A733_GPU_PLATFORM_OFF))
        return 0;
    p->deadline = deadline;
    if (!clock_ok(p)) return 0;
    p->phase = A733_GPU_PLATFORM_ADMIT;
    return 1;
}
int a733_gpu_platform_bind_pmic(struct a733_gpu_platform *p,struct a733_gpu_resource_native *n,
    struct a733_pmic *provider,const struct a733_pmic_lifetime *life,uint64_t owner)
{
    if(!p || !p->initialized || p->phase!=A733_GPU_PLATFORM_NEW || p->pmic_native ||
        !n || !life || !provider || provider->self!=provider || provider->rails->binding.gpu!=p->manager ||
        !a733_extents_disjoint_checked((uintptr_t)p,sizeof(*p),(uintptr_t)n,sizeof(*n)) ||
        !a733_pmic_consumer_storage(provider,p,sizeof(*p)) ||
        !a733_gpu_resources_native_use_pmic(n,p->manager,provider))return 0;
    const struct a733_pmic_consumer_config config={provider,*life,owner,A733_RAIL_DCDC4};
    if(!a733_pmic_consumer_init(&p->pmic,&config) || !a733_gpu_resources_native_bind_view(n,&p->pmic,p->client))return 0;
    /* GPU_TOP: the row on the provider's power-domain owner, under the same
     * owner and the consumer's DCDC4 token. */
    if(provider->domains && !a733_power_domain_row_init(&p->power.row,provider->domains,5,owner,&p->pmic.rail))return 0;
    p->pmic_native=n;return 1;
}
/* DCDC4 to the GPU operating point, once, before the supply consumer's first
 * sample: 1 done, 0 pending, -1 failed with the lease and request retained. */
enum { RAISE_LEASE, RAISE_LEASED, RAISE_READ, RAISE_WRITE, RAISE_RELEASE, RAISE_SETTLE, RAISE_DONE };
static int raise_result(struct a733_gpu_platform *p, unsigned written)
{
    struct a733_pmic *provider = p->pmic.config.provider;
    struct a733_pmic_snapshot snap = {0};
    enum a733_pmic_status r = a733_pmic_result(provider, &p->supply_request, &snap);
    if (r == A733_PMIC_BUSY || r == A733_PMIC_PENDING) return 0;
    const unsigned code = snap.selector & 127;
    if (r != A733_PMIC_COMPLETE || snap.rail != A733_RAIL_DCDC4 || snap.page_before || snap.page_after ||
        snap.chip != 3 || snap.revision || !snap.enabled || !snap.decoded || !(snap.enable & 8) ||
        (written ? code != A733_GPU_DCDC4_SELECTOR || snap.programmed_uv != A733_GPU_SUPPLY_UV :
                   code != A733_GPU_DCDC4_SELECTOR && code != A733_GPU_DCDC4_BOOT)) return -1;
    r = a733_pmic_reap(provider, &p->supply_request);
    if (r == A733_PMIC_BUSY) return 0;
    if (r != A733_PMIC_COMPLETE) return -1;
    p->supply_request = (struct a733_pmic_request){0};
    return code == A733_GPU_DCDC4_SELECTOR ? 1 : 2;
}
static int supply_raise(struct a733_gpu_platform *p)
{
    struct a733_pmic *provider = p->pmic.config.provider;
    struct a733_rail_leases *rails = provider->rails;
    const uint64_t owner = p->pmic.config.owner;
    const a733_rail_mask_t dcdc4 = A733_RAIL_BIT(A733_RAIL_DCDC4);
    enum a733_rail_result lease;
    enum a733_pmic_status status;
    int r;
    switch (p->supply_stage) {
    case RAISE_LEASE:
        lease = a733_rail_acquire(rails, owner, 0, dcdc4, p->deadline, &p->supply_token);
        if (lease == A733_RAIL_BUSY) return 0;
        if ((lease != A733_RAIL_PENDING && lease != A733_RAIL_READY) || !p->supply_token.id) return -1;
        p->supply_stage = RAISE_LEASED;
        return 0;
    case RAISE_LEASED:
        lease = a733_rail_poll(rails, &p->supply_token);
        if (lease == A733_RAIL_BUSY || lease == A733_RAIL_PENDING) return 0;
        if (lease != A733_RAIL_READY) return -1;
        status = a733_pmic_submit(provider, owner, &p->supply_token, A733_RAIL_DCDC4, p->deadline,
                                  &p->supply_request);
        if (status == A733_PMIC_BUSY || (status == A733_PMIC_PENDING && !p->supply_request.id)) return 0;
        if (status != A733_PMIC_PENDING) return -1;
        p->supply_stage = RAISE_READ;
        return 0;
    case RAISE_READ:
        r = raise_result(p, 0);
        if (r <= 0) return r;
        if (r == 1) { p->supply_stage = RAISE_RELEASE; return 0; }
        status = a733_pmic_submit_write(provider, owner, &p->supply_token, A733_RAIL_DCDC4,
            A733_GPU_DCDC4_BOOT, A733_GPU_DCDC4_SELECTOR, p->deadline, &p->supply_request);
        if (status == A733_PMIC_BUSY || (status == A733_PMIC_PENDING && !p->supply_request.id)) return 0;
        if (status != A733_PMIC_PENDING) return -1;
        p->supply_stage = RAISE_WRITE;
        return 0;
    case RAISE_WRITE:
        r = raise_result(p, 1);
        if (r <= 0) return r;
        if (!a733_deadline_after(p->last_now, A733_GPU_SUPPLY_SETTLE_US, &p->supply_settled)) return -1;
        p->supply_written = 1;
        p->supply_stage = RAISE_RELEASE;
        return 0;
    case RAISE_RELEASE:
        lease = a733_rail_release(rails, &p->supply_token, p->deadline);
        if (lease == A733_RAIL_BUSY) return 0;
        if (lease != A733_RAIL_READY) return -1;
        p->supply_token = (struct a733_rail_token){0};
        p->supply_stage = p->supply_written ? RAISE_SETTLE : RAISE_DONE;
        return p->supply_written ? 0 : 1;
    case RAISE_SETTLE:
        /* The clock may round down; passing the time by more than the
         * settling interval guarantees it. */
        if (p->last_now <= p->supply_settled) return 0;
        p->supply_stage = RAISE_DONE;
        return 1;
    case RAISE_DONE:
        return 1;
    }
    return -1;
}
static int pmic_health(struct a733_gpu_platform *p)
{
    if(!p->pmic_native)return 1;
    if(p->pmic_native->pmic_view!=&p->pmic || p->pmic_native->pmic_manager!=p->manager ||
        p->pmic_native->pmic_client!=p->client ||
        p->pmic_native->pmic!=p->pmic.config.provider ||
        !a733_gpu_resources_native_use_pmic(p->pmic_native,p->manager,p->pmic.config.provider))return fail(p);
    int r=a733_pmic_consumer_health(&p->pmic);return r<0 ? fail(p) : r;
}
int a733_gpu_platform_poll(void *context)
{
    struct a733_gpu_platform *p = context;
    if (!p || !p->initialized || p->phase == A733_GPU_PLATFORM_FAILED) return -1;
    if (p->phase == A733_GPU_PLATFORM_ON) return pmic_health(p);
    if (p->phase != A733_GPU_PLATFORM_ADMIT && p->phase != A733_GPU_PLATFORM_STARTING) return -1;
    if (!clock_ok(p)) return -1;
    if (p->phase == A733_GPU_PLATFORM_ADMIT) {
        if(p->pmic_native) {
            int r=p->pmic_observed ? 1 : supply_raise(p);
            if(r<0)return fail(p);
            if(!r)return 0;
            if(!clock_ok(p))return -1;
            r=p->pmic_observed ? pmic_health(p) : a733_pmic_consumer_sample(&p->pmic,p->deadline);
            if(r<0)return fail(p);
            if(!r)return 0;
            p->pmic_observed=1;
            r=a733_pmic_consumer_arm(&p->pmic,p->deadline);
            if(r<0)return fail(p);
            if(!r)return 0;
        }
        p->resource_result = a733_gpu_resources_acquire(p->manager, p->client, A733_GPU_RESOURCE_ALL,
                                                       p->deadline, &p->token, &p->ready);
        if(p->pmic_native)p->pmic.armed=0;
        if (!clock_ok(p)) return -1;
        if (p->resource_result == A733_GPU_RESOURCE_BUSY) return 0;
        if (p->resource_result != A733_GPU_RESOURCE_OK) return fail(p);
        p->phase = A733_GPU_PLATFORM_STARTING;
        if (a733_gpu_power_on(&p->power, p->deadline) != A733_GPU_POWER_AGAIN) return fail(p);
        return 0;
    }
    int health=pmic_health(p);if(health!=1)return health;
    enum a733_gpu_power_result result = a733_gpu_power_poll(&p->power);
    if (!clock_ok(p)) return -1;
    if (result == A733_GPU_POWER_AGAIN) return 0;
    if (result != A733_GPU_POWER_OK) return fail(p);
    health=pmic_health(p);if(health!=1)return health;
    p->phase = A733_GPU_PLATFORM_ON;
    return 1;
}
int a733_gpu_platform_off_begin(struct a733_gpu_platform *p, uint64_t deadline)
{
    if (!p || !p->initialized || p->phase != A733_GPU_PLATFORM_ON) return 0;
    p->deadline = deadline;
    if (!clock_ok(p)) return 0;
    p->phase = A733_GPU_PLATFORM_STOPPING;
    if (a733_gpu_power_off(&p->power, deadline) != A733_GPU_POWER_AGAIN) { fail(p); return 0; }
    return 1;
}
int a733_gpu_platform_off_poll(struct a733_gpu_platform *p)
{
    if (!p || !p->initialized || p->phase == A733_GPU_PLATFORM_FAILED) return -1;
    if (p->phase == A733_GPU_PLATFORM_OFF) return 1;
    if (p->phase != A733_GPU_PLATFORM_STOPPING && p->phase != A733_GPU_PLATFORM_RELEASE) return -1;
    if (!clock_ok(p)) return -1;
    int health=pmic_health(p);if(health!=1)return health;
    if (p->phase == A733_GPU_PLATFORM_STOPPING) {
        enum a733_gpu_power_result result = a733_gpu_power_poll(&p->power);
        if (!clock_ok(p)) return -1;
        if (result == A733_GPU_POWER_AGAIN) return 0;
        if (result != A733_GPU_POWER_OK) return fail(p);
        p->phase = A733_GPU_PLATFORM_RELEASE;
        return 0;
    }
    p->resource_result = a733_gpu_resources_release(p->manager, p->client, p->token, p->deadline);
    /* Record completed release even if the following clock read fails. */
    if (p->resource_result == A733_GPU_RESOURCE_OK) p->token = 0;
    if (!clock_ok(p)) return -1;
    if (p->resource_result == A733_GPU_RESOURCE_BUSY) return 0;
    if (p->resource_result != A733_GPU_RESOURCE_OK) return fail(p);
    p->phase = A733_GPU_PLATFORM_OFF;
    return 1;
}
