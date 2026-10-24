#include "a733_gpu_resources.h"
#include "a733_range.h"
#include "a733_clock_reference.h"
#include <stddef.h>

#define CCU UINT32_C(0x02002000)
#define RCCU UINT32_C(0x07010000)
#define RTC UINT32_C(0x07090000)
#define PCK UINT32_C(0x07060000)
static uint32_t expand(uint32_t mask)
{
    if (mask & (A733_GPU_RESOURCE_PARENT | A733_GPU_RESOURCE_PCK |
                A733_GPU_RESOURCE_AHB | A733_GPU_RESOURCE_MBUS)) mask |= A733_GPU_RESOURCE_OSC;
    return mask;
}
static int valid(unsigned client, uint32_t mask)
{
    return client < A733_GPU_RESOURCE_CLIENTS && mask && !(mask & ~A733_GPU_RESOURCE_ALL) &&
        (!(mask & A733_GPU_RESOURCE_LOCAL) || mask == A733_GPU_RESOURCE_ALL);
}
static enum a733_gpu_resource_result enter(struct a733_gpu_resource_manager *m)
{
    if (!m || !m->initialized) return A733_GPU_RESOURCE_INVALID;
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&m->busy, &expected, 1,
            memory_order_acquire, memory_order_relaxed)) return A733_GPU_RESOURCE_BUSY;
    if (m->failed) {
        atomic_store_explicit(&m->busy, 0, memory_order_release);
        return A733_GPU_RESOURCE_FAULT;
    }
    return A733_GPU_RESOURCE_OK;
}
static enum a733_gpu_resource_result leave(struct a733_gpu_resource_manager *m,
                                           enum a733_gpu_resource_result result)
{ atomic_store_explicit(&m->busy, 0, memory_order_release); return result; }
enum a733_gpu_resource_result a733_gpu_resources_status(struct a733_gpu_resource_manager *m,
    unsigned client,const struct a733_gpu_resource_lease *token)
{
    if(client>=A733_GPU_RESOURCE_CLIENTS || !a733_ranges_disjoint(m,sizeof(*m),token,sizeof(*token)))
        return A733_GPU_RESOURCE_INVALID;
    enum a733_gpu_resource_result r=enter(m);
    if(r!=A733_GPU_RESOURCE_OK)return r;
    const struct a733_gpu_resource_lease *held=&m->leases[client];
    int exact=token->token && token->mode==A733_GPU_RESOURCE_STABLE &&
        token->token==held->token && token->mask==held->mask && token->mode==held->mode;
    return leave(m,exact ? A733_GPU_RESOURCE_OK : A733_GPU_RESOURCE_INVALID);
}
static int fail(struct a733_gpu_resource_manager *m, enum a733_gpu_resource_error error)
{ m->failed = 1; m->error = error; return 0; }
static int time_ok(struct a733_gpu_resource_manager *m)
{
    uint64_t now;
    if (!m->ops.now(m->context, &now)) return fail(m, A733_GPU_RESOURCE_IO);
    if (now < m->last_now) return fail(m, A733_GPU_RESOURCE_CLOCK);
    m->last_now = now;
    if (now >= m->deadline) return fail(m, A733_GPU_RESOURCE_TIMEOUT);
    return 1;
}
static int read32(struct a733_gpu_resource_manager *m, uintptr_t address, uint32_t *out)
{
    ++m->stage;
    if (!time_ok(m)) return 0;
    if (!m->ops.read32(m->context, address, out)) return fail(m, A733_GPU_RESOURCE_IO);
    m->last_value = *out;
    return time_ok(m);
}
static int check(struct a733_gpu_resource_manager *m, uintptr_t address,
                 uint32_t mask, uint32_t value, enum a733_gpu_resource_error error)
{
    uint32_t v;
    return read32(m, address, &v) && ((v & mask) == value || fail(m, error));
}
static int gate(struct a733_gpu_resource_manager *m, uintptr_t address, uint32_t bit, uint32_t key, int may_enable)
{
    uint32_t v;
    if (!read32(m, address, &v)) return 0;
    if (!(v & bit)) {
        if (!may_enable) return fail(m, A733_GPU_RESOURCE_GATE);
        if ((v & (key & ~bit)) != (key & ~bit)) return fail(m, A733_GPU_RESOURCE_GATE);
        ++m->stage;
        if (!time_ok(m)) return 0;
        if (!m->ops.update32(m->context, address, key & ~bit, bit | key, m->deadline))
            return fail(m, A733_GPU_RESOURCE_IO);
        if (!time_ok(m)) return 0;
        if (!check(m, address, bit, bit, A733_GPU_RESOURCE_GATE)) return 0;
    }
    return 1;
}
static int clocks(struct a733_gpu_resource_manager *m, uint32_t mask,
                  struct a733_gpu_power_resources *ready)
{
    uint32_t osc, ref, peri, sdm0, sdm1, gate_config, gate_status;
    if (!(mask & A733_GPU_RESOURCE_OSC)) return 1;
    if (!read32(m, RTC + 0x160, &osc)) return 0;
    /* OSC ownership protects the detected input; consumers below use PLL_REF. */
    if ((osc & UINT32_C(0x10000002)) != UINT32_C(0x10000002))
        return fail(m, A733_GPU_RESOURCE_OSCILLATOR);
    if (!(mask & A733_GPU_RESOURCE_PARENT)) return 1;
    if (!read32(m, CCU, &ref) || !read32(m, CCU + 0xa0, &peri) ||
        !read32(m, CCU + 0xa8, &sdm0) || !read32(m, CCU + 0xac, &sdm1) ||
        !read32(m, CCU + 0x1908, &gate_config) || !read32(m, CCU + 0x1988, &gate_status)) return 0;
    /* Manual 4.1.6.249/258: PERI0 2X, 400M-all and 400M must each
     * remain No-Auto, software enabled, and effectively enabled. */
    const uint32_t branches = (1u << 11) | (1u << 2) | (1u << 1);
    const uint32_t enabled = branches << 16, required = branches | enabled;
    if ((gate_config & required) != required || (gate_status & enabled) != enabled)
        return fail(m, A733_GPU_RESOURCE_PLL);
    /* User Manual 4.1.6.6: inherited rate calculation requires M1=1. */
    if ((ref & 0xf8000000) != 0xf8000000 || (peri & 0xfe000000) != 0xfe000000 ||
        (peri & 2) || (sdm0 & 0x80000000) || (sdm1 & 0x08000000))
        return fail(m, A733_GPU_RESOURCE_PLL);
    /* PERI0 has fixed PLL_REF ancestry, not raw DCXO. */
    uint64_t nperi = ((peri >> 8) & 255) + 1, mperi = ((peri >> 20) & 7) + 1;
    if (!a733_clock_ref24_valid(osc, ref) || nperi < 53 || nperi > 105 ||
        UINT64_C(24000000) * nperi != (uint64_t)A733_GPU_INITIAL_HZ * mperi * 3)
        return fail(m, A733_GPU_RESOURCE_PLL);
    ready->parent_hz = A733_GPU_INITIAL_HZ;
    return 1;
}
static int supply(struct a733_gpu_resource_manager *m, uint32_t mask,
                  struct a733_gpu_power_resources *ready)
{
    if (!(mask & A733_GPU_RESOURCE_RAIL)) return 1;
    ++m->stage;
    if (!time_ok(m)) return 0;
    if (!m->ops.rail(m->context, &m->rail, m->deadline)) return fail(m, A733_GPU_RESOURCE_IO);
    if (!time_ok(m)) return 0;
    const struct a733_gpu_resource_rail *r = &m->rail;
    if (r->page_before || r->page_after || r->chip != 3 || r->revision != 0 ||
        !(r->enable & 8) || (r->voltage & 127) != 30) return fail(m, A733_GPU_RESOURCE_SUPPLY);
    ready->supply_uv = A733_GPU_INITIAL_UV;
    return 1;
}
int a733_gpu_resources_init(struct a733_gpu_resource_manager *m,
                            const struct a733_gpu_resource_ops *ops, void *context)
{
    if (!m || !ops || m->initialized || !ops->now || !ops->read32 || !ops->update32 || !ops->rail)
        return 0;
    uintptr_t a = (uintptr_t)m, b = (uintptr_t)ops;
    if (a > UINTPTR_MAX - sizeof(*m) || b > UINTPTR_MAX - sizeof(*ops) ||
        !(a + sizeof(*m) <= b || b + sizeof(*ops) <= a)) return 0;
    *m = (struct a733_gpu_resource_manager){.ops = *ops, .context = context,
                                          .next_token = 1, .initialized = 1};
    atomic_init(&m->busy, 0);
    return 1;
}
static enum a733_gpu_resource_result reserve(struct a733_gpu_resource_manager *m,
    unsigned client, uint32_t mask, enum a733_gpu_resource_mode mode, uint64_t *token)
{
    if (m->leases[client].mode != A733_GPU_RESOURCE_FREE || !m->next_token)
        return A733_GPU_RESOURCE_BUSY;
    for (unsigned i = 0; i < A733_GPU_RESOURCE_SLOTS; ++i) {
        struct a733_gpu_resource_lease *l = &m->leases[i];
        uint32_t common = mask & l->mask;
        if (common && (mode == A733_GPU_RESOURCE_CHANGE || l->mode == A733_GPU_RESOURCE_CHANGE ||
                       (common & A733_GPU_RESOURCE_LOCAL))) return A733_GPU_RESOURCE_BUSY;
    }
    *token = m->next_token++;
    m->leases[client] = (struct a733_gpu_resource_lease){*token, mask, mode};
    return A733_GPU_RESOURCE_OK;
}
enum a733_gpu_resource_result a733_gpu_resources_acquire(
    struct a733_gpu_resource_manager *m, unsigned client, uint32_t mask,
    uint64_t deadline, uint64_t *token, struct a733_gpu_power_resources *ready)
{
    if (!valid(client, mask) || !a733_ranges_disjoint(m, sizeof(*m), token, sizeof(*token)) ||
        !a733_ranges_disjoint(m, sizeof(*m), ready, sizeof(*ready)) ||
        !a733_ranges_disjoint(token, sizeof(*token), ready, sizeof(*ready))) return A733_GPU_RESOURCE_INVALID;
    enum a733_gpu_resource_result result = enter(m);
    if (result != A733_GPU_RESOURCE_OK) return result;
    mask = expand(mask);
    result = reserve(m, client, mask, A733_GPU_RESOURCE_STABLE, token);
    if (result != A733_GPU_RESOURCE_OK) return leave(m, result);
    m->deadline = deadline; m->stage = 0;
    struct a733_gpu_power_resources r = {0};
    int ok = time_ok(m) && clocks(m, mask, &r) && supply(m, mask, &r);
    if (ok && (mask & A733_GPU_RESOURCE_LOCAL))
        ok = check(m, CCU + 0xb20, 0x80000000, 0, A733_GPU_RESOURCE_LIVE) &&
             check(m, CCU + 0xb24, 0x10001, 0, A733_GPU_RESOURCE_LIVE);
    if (ok && (mask & A733_GPU_RESOURCE_PCK)) ok = gate(m, RCCU + 0x1ac, 1, 0, 1);
    if (ok && (mask & A733_GPU_RESOURCE_AHB)) ok = gate(m, CCU + 0x5c0, 0x80, 0x010000ff, mask & A733_GPU_RESOURCE_LOCAL);
    if (ok && (mask & A733_GPU_RESOURCE_MBUS)) ok = gate(m, CCU + 0x5e0, 0x10000, 0x41055800, mask & A733_GPU_RESOURCE_LOCAL);
    if (!ok) {
        m->leases[client].mode = A733_GPU_RESOURCE_RETAINED;
        return leave(m, A733_GPU_RESOURCE_FAULT);
    }
    *ready = r;
    return leave(m, A733_GPU_RESOURCE_OK);
}
enum a733_gpu_resource_result a733_gpu_resources_change(
    struct a733_gpu_resource_manager *m, unsigned client, uint32_t mask, uint64_t *token)
{
    if (!valid(client, mask) || !a733_ranges_disjoint(m, sizeof(*m), token, sizeof(*token)))
        return A733_GPU_RESOURCE_INVALID;
    enum a733_gpu_resource_result result = enter(m);
    if (result != A733_GPU_RESOURCE_OK) return result;
    return leave(m, reserve(m, client, mask, A733_GPU_RESOURCE_CHANGE, token));
}
enum a733_gpu_resource_result a733_gpu_resources_change_end(
    struct a733_gpu_resource_manager *m, unsigned client, uint64_t token, int success)
{
    if (client >= A733_GPU_RESOURCE_CLIENTS || !token || (success != 0 && success != 1))
        return A733_GPU_RESOURCE_INVALID;
    enum a733_gpu_resource_result result = enter(m);
    if (result != A733_GPU_RESOURCE_OK) return result;
    struct a733_gpu_resource_lease *l = &m->leases[client];
    if ((m->rail_guards & (1u << client)) || l->mode != A733_GPU_RESOURCE_CHANGE || l->token != token)
        return leave(m, A733_GPU_RESOURCE_INVALID);
    if (!success) { l->mode = A733_GPU_RESOURCE_RETAINED; fail(m, A733_GPU_RESOURCE_IO); }
    else *l = (struct a733_gpu_resource_lease){0};
    return leave(m, success ? A733_GPU_RESOURCE_OK : A733_GPU_RESOURCE_FAULT);
}
enum a733_gpu_resource_result a733_gpu_resources_release(
    struct a733_gpu_resource_manager *m, unsigned client, uint64_t token, uint64_t deadline)
{
    if (client >= A733_GPU_RESOURCE_CLIENTS || !token) return A733_GPU_RESOURCE_INVALID;
    enum a733_gpu_resource_result result = enter(m);
    if (result != A733_GPU_RESOURCE_OK) return result;
    struct a733_gpu_resource_lease *l = &m->leases[client];
    if ((m->rail_guards & (1u << client)) || l->mode != A733_GPU_RESOURCE_STABLE || l->token != token)
        return leave(m, A733_GPU_RESOURCE_INVALID);
    m->deadline = deadline; m->stage = 0;
    int ok = time_ok(m);
    if (ok && (l->mask & A733_GPU_RESOURCE_LOCAL))
        ok = check(m, CCU + 0xb20, 0x80000000, 0, A733_GPU_RESOURCE_LIVE) &&
             check(m, CCU + 0xb24, 0x10001, 0, A733_GPU_RESOURCE_LIVE) &&
             check(m, PCK + 0x5008, 15, 0, A733_GPU_RESOURCE_LIVE) &&
             check(m, PCK + 0x6000, UINT32_MAX, 0, A733_GPU_RESOURCE_LIVE) &&
             check(m, PCK + 0x6008, UINT32_MAX, 0, A733_GPU_RESOURCE_LIVE);
    if (!ok) { l->mode = A733_GPU_RESOURCE_RETAINED; return leave(m, A733_GPU_RESOURCE_FAULT); }
    *l = (struct a733_gpu_resource_lease){0};
    return leave(m, A733_GPU_RESOURCE_OK);
}

static int exact_guard(struct a733_gpu_resource_manager *m, const struct a733_gpu_rail_guard *g)
{
    if (g->client < A733_GPU_RESOURCE_CLIENTS || g->client >= A733_GPU_RESOURCE_SLOTS || !(m->rail_guards & (1u << g->client))) return 0;
    const struct a733_gpu_resource_lease *l = &m->leases[g->client];
    return g->lease.token && g->lease.token == l->token &&
        g->lease.mask == A733_GPU_RESOURCE_RAIL && g->lease.mask == l->mask &&
        g->lease.mode == l->mode &&
        (l->mode == A733_GPU_RESOURCE_STABLE || l->mode == A733_GPU_RESOURCE_CHANGE);
}
enum a733_gpu_resource_result a733_gpu_rail_guard_acquire(
    struct a733_gpu_resource_manager *m, int change, struct a733_gpu_rail_guard *out)
{
    if ((change != 0 && change != 1) || !a733_ranges_disjoint(m,sizeof(*m),out,sizeof(*out)))
        return A733_GPU_RESOURCE_INVALID;
    enum a733_gpu_resource_result r = enter(m);
    if (r != A733_GPU_RESOURCE_OK) return r;
    unsigned client = A733_GPU_RESOURCE_CLIENTS;
    while (client < A733_GPU_RESOURCE_SLOTS && m->leases[client].mode != A733_GPU_RESOURCE_FREE) ++client;
    if (client == A733_GPU_RESOURCE_SLOTS) return leave(m,A733_GPU_RESOURCE_BUSY);
    uint64_t token;
    r = reserve(m,client,A733_GPU_RESOURCE_RAIL,
        change ? A733_GPU_RESOURCE_CHANGE : A733_GPU_RESOURCE_STABLE,&token);
    if (r == A733_GPU_RESOURCE_OK) {
        m->rail_guards |= 1u << client;
        *out = (struct a733_gpu_rail_guard){m->leases[client],client};
    }
    return leave(m,r);
}
enum a733_gpu_resource_result a733_gpu_rail_guard_status(
    struct a733_gpu_resource_manager *m, const struct a733_gpu_rail_guard *g)
{
    if (!a733_ranges_disjoint(m,sizeof(*m),g,sizeof(*g))) return A733_GPU_RESOURCE_INVALID;
    enum a733_gpu_resource_result r = enter(m);
    if (r != A733_GPU_RESOURCE_OK) return r;
    return leave(m,exact_guard(m,g) ? A733_GPU_RESOURCE_OK : A733_GPU_RESOURCE_INVALID);
}
enum a733_gpu_resource_result a733_gpu_rail_guard_end(
    struct a733_gpu_resource_manager *m, const struct a733_gpu_rail_guard *g)
{
    if (!a733_ranges_disjoint(m,sizeof(*m),g,sizeof(*g))) return A733_GPU_RESOURCE_INVALID;
    enum a733_gpu_resource_result r = enter(m);
    if (r != A733_GPU_RESOURCE_OK) return r;
    if (!exact_guard(m,g)) return leave(m,A733_GPU_RESOURCE_INVALID);
    m->leases[g->client] = (struct a733_gpu_resource_lease){0};
    m->rail_guards &= ~(1u << g->client);
    return leave(m,A733_GPU_RESOURCE_OK);
}
