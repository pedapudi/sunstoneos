#include "a733_i2s_platform.h"
#include "a733_audio_leases.h"

#define CCU UINT32_C(0x02002000)
#define OSC UINT32_C(0x07090160)
#define AUDIO (CCU + 0x280u)
#define MODULE (CCU + 0x1200u)
#define BUS (CCU + 0x120cu)
#define BIT(n) (UINT32_C(1) << (n))
#define CONTROLS (BIT(31)|BIT(30)|BIT(29)|BIT(27))
#define RATE_MASK (UINT32_C(0xff00)|UINT32_C(0x70000))
#define RATE_VALUE (UINT32_C(127)<<8 | UINT32_C(4)<<16)
#define MODULE_MASK (BIT(31)|UINT32_C(0x07000000)|UINT32_C(31))
#define MODULE_VALUE (UINT32_C(2)<<24 | UINT32_C(24))
#define BUS_MASK (BIT(16)|BIT(0))
#define AUDIO_GATE_MODE (BIT(1)|BIT(2))
#define AUDIO_GATE_ENABLE (BIT(17)|BIT(18))
#define READY A733_I2S_RESOURCE_READY
#define BUSY A733_I2S_RESOURCE_BUSY
#define PENDING A733_I2S_RESOURCE_PENDING
#define RETAINED A733_I2S_RESOURCE_RETAINED
/* Stages are private; zero is a fresh object. */
enum { P_REF, P_AUDIO, P_CHECK, P_LOCK, P_SETTLE, P_GATES, P_PUBLISH, P_READY };
enum { L_IDLE, L_LEASE, L_CHECK, L_PINS, L_RESET_WAIT, L_ENABLE, L_READY,
       L_STOP, L_UNPIN, L_RELEASE };

static int time_ok(struct a733_audio_parent *p, uint64_t deadline)
{
    uint64_t now;
    if (!p->ops.now_us(p->cookie, &now) || now < p->last_time || now >= deadline) return 0;
    p->last_time = now;
    return 1;
}
static enum a733_i2s_resource_result parent_fail(struct a733_audio_parent *p)
{ p->failed = 1; return RETAINED; }
static enum a733_i2s_resource_result local_fail(struct a733_i2s_platform *s)
{ s->failed = 1; return RETAINED; }
/* 0: lock busy without I/O; -1: uncertainty; 1: owns lock. */
static int enter(struct a733_audio_parent *p, uint64_t deadline)
{
    if (!time_ok(p, deadline)) return -1;
    if (!p->ops.enter(p->cookie, deadline)) return time_ok(p, deadline) ? 0 : -1;
    if (time_ok(p, deadline)) return 1;
    p->ops.leave(p->cookie);
    return -1;
}
static int rd(struct a733_audio_parent *p, uint64_t deadline, uintptr_t a, uint32_t *v)
{
    return time_ok(p, deadline) && p->ops.read(p->cookie, a, v) && time_ok(p, deadline);
}
static int update(struct a733_audio_parent *p, uint64_t deadline, uintptr_t a,
                  uint32_t mask, uint32_t value)
{
    uint32_t old, got;
    if (!rd(p, deadline, a, &old)) return 0;
    uint32_t next = (old & ~mask) | (value & mask);
    if (next == old) return 1;
    return p->ops.write(p->cookie, a, next) && time_ok(p, deadline) &&
           rd(p, deadline, a, &got) && (got & mask) == (next & mask) &&
           (got & ~(mask | (a == AUDIO ? BIT(28) : 0))) ==
           (old & ~(mask | (a == AUDIO ? BIT(28) : 0)));
}
static int reference(struct a733_audio_parent *p)
{
    uint32_t osc, ref;
    return rd(p, p->deadline, OSC, &osc) &&
        (((osc >> 14) & 3) == 0 || ((osc >> 14) & 3) == 3) &&
        rd(p, p->deadline, CCU, &ref) && (ref & UINT32_C(0xf8000000)) == UINT32_C(0xf8000000) &&
        ((ref >> 8) & 255) >= 79 && ((ref >> 8) & 255) <= 115 &&
        ((ref >> 8) & 255) == ((ref >> 16) & 127);
}
static int gate_control(struct a733_audio_parent *p)
{
    uint32_t value;
    /* Manual 4.1.6.254: low bits set mean No-Auto; upper bits enable
     * the outputs. Preserve the existing policy covering DIV2 and DIV5. */
    return rd(p,p->deadline,CCU+0x191c,&value) &&
        (value & (AUDIO_GATE_MODE|AUDIO_GATE_ENABLE)) == (AUDIO_GATE_MODE|AUDIO_GATE_ENABLE);
}
static int gate_status(struct a733_audio_parent *p, unsigned *enabled)
{
    uint32_t value;
    if (!rd(p,p->deadline,CCU+0x199c,&value)) return 0;
    *enabled = (value & AUDIO_GATE_ENABLE) == AUDIO_GATE_ENABLE;
    return 1;
}
static int baseline(struct a733_audio_parent *p)
{
    uint32_t x, y;
    if (!reference(p) || !rd(p, p->deadline, AUDIO, &p->audio_value) ||
        (p->audio_value & BIT(24)) || !rd(p, p->deadline, CCU+0x288, &x) || (x & BIT(31)) ||
        !rd(p, p->deadline, CCU+0x28c, &y) || (y & BIT(27)) ||
        !gate_control(p)) return 0;
    if (!p->cold) {
        unsigned enabled;
        return (p->audio_value & (CONTROLS|BIT(28)|RATE_MASK)) ==
            (CONTROLS|BIT(28)|RATE_VALUE) && gate_status(p,&enabled) && enabled;
    }
    if (p->audio_value & CONTROLS) return 0;
    static const unsigned consumers[] = {0x1200,0x1210,0x1220,0x1224,0x1230,0x1240,0x1280,0x12c0};
    for (unsigned i=0; i<sizeof(consumers)/sizeof(*consumers); ++i)
        if (!rd(p, p->deadline, CCU+consumers[i], &x) || (x & BIT(31))) return 0;
    return update(p,p->deadline,AUDIO,RATE_MASK,RATE_VALUE) &&
        update(p,p->deadline,AUDIO,BIT(30),BIT(30)) &&
        update(p,p->deadline,AUDIO,BIT(31),BIT(31)) &&
        update(p,p->deadline,AUDIO,BIT(29),BIT(29));
}
static enum a733_i2s_resource_result lease(struct a733_audio_parent *p,
    enum a733_audio_scope scope, struct a733_audio_token *token, uint64_t deadline)
{
    if (!time_ok(p,deadline)) return RETAINED;
    struct a733_audio_token old = *token;
    enum a733_i2s_resource_result r = p->ops.acquire(p->cookie,scope,token,deadline);
    if ((old.id && (token->id != old.id || token->scope != old.scope)) ||
        (r == BUSY && (token->id != old.id || token->scope != old.scope))) {
        if (old.id) *token = old;
        return RETAINED;
    }
    if (r == RETAINED || !time_ok(p,deadline)) return RETAINED;
    if (r == BUSY) return BUSY;
    if ((r != READY && r != PENDING) || !token->id || token->scope != scope) return RETAINED;
    return r;
}
int a733_audio_parent_init(struct a733_audio_parent *p, const struct a733_audio_operations *o,
                          void *cookie, unsigned cold, uint64_t deadline)
{
    if (!p || p->initialized || !o || !o->acquire || !o->publish || !o->release ||
        !o->enter || !o->leave || !o->now_us || !o->read || !o->write || cold > 1 || !deadline)
        return 0;
    p->ops = *o; p->cookie = cookie; p->cold = cold; p->deadline = deadline;
    p->initialized = 1;
    return 1;
}
int a733_audio_parent_ready(const struct a733_audio_parent *p)
{ return p && p->initialized && !p->failed && p->stage == P_READY; }
enum a733_i2s_resource_result a733_audio_parent_poll(struct a733_audio_parent *p)
{
    if (!p || !p->initialized || p->failed) return RETAINED;
    if (p->stage == P_READY) return READY;
    if (!time_ok(p,p->deadline)) return parent_fail(p);
    enum a733_i2s_resource_result r;
    if (p->stage == P_REF || p->stage == P_AUDIO) {
        r = p->stage == P_REF ? lease(p,A733_AUDIO_REFERENCE,&p->reference,p->deadline) :
            lease(p,p->cold ? A733_AUDIO_CHANGE : A733_AUDIO_STABLE,&p->audio,p->deadline);
        if (r == RETAINED) return parent_fail(p);
        if (r != READY) return PENDING;
        ++p->stage;
        return PENDING;
    }
    if (p->stage == P_PUBLISH) {
        r = p->ops.publish(p->cookie,&p->audio,&p->published,p->deadline);
        if (r == RETAINED || !time_ok(p,p->deadline)) return parent_fail(p);
        if (r == BUSY || r == PENDING) return PENDING;
        if (r != READY || !p->published.id || p->published.scope != A733_AUDIO_STABLE)
            return parent_fail(p);
        p->audio = p->published;
        p->published = (struct a733_audio_token){0};
        p->stage = P_READY;
        return READY;
    }
    int held = enter(p,p->deadline);
    if (held < 0) return parent_fail(p);
    if (!held) return PENDING;
    int ok = 1;
    if (p->stage == P_CHECK) {
        ok = baseline(p);
        if (ok) { p->stage = p->cold ? P_LOCK : P_READY; p->wait_since = p->last_time; }
    } else if (p->stage == P_LOCK && p->last_time - p->wait_since >= 3) {
        uint32_t v;
        ok = rd(p,p->deadline,AUDIO,&v);
        if (ok) {
            p->locked_samples = v & BIT(28) ? p->locked_samples + 1 : 0;
            p->wait_since = p->last_time;
            if (p->locked_samples == 3) p->stage = P_SETTLE;
        }
    } else if (p->stage == P_SETTLE && p->last_time - p->wait_since >= 20) {
        uint32_t v;
        ok = rd(p,p->deadline,AUDIO,&v) && (v & BIT(28)) &&
             update(p,p->deadline,AUDIO,BIT(27),BIT(27));
        if (ok) p->stage = P_GATES;
    } else if (p->stage == P_GATES) {
        unsigned enabled;
        /* Effective status is meaningful here after PLL output enable.
         * Keep the change lease while propagation is pending. */
        ok = gate_control(p) && gate_status(p,&enabled);
        if (ok && enabled) p->stage = P_PUBLISH;
    }
    p->ops.leave(p->cookie);
    if (!ok || !time_ok(p,p->deadline)) return parent_fail(p);
    return p->stage == P_READY ? READY : PENDING;
}

static int init_route(struct a733_i2s_platform *s, struct a733_audio_parent *parent,
    struct a733_pins *pins, uint64_t owner, const struct a733_pin_request *requests, size_t count, unsigned data_pin)
{
    if (!s || s->initialized || !parent || !parent->initialized || !pins || !pins->initialized ||
        !owner || !requests || (count != 3 && count != 4)) return 0;
    uint32_t mask = 0;
    for (size_t i=0;i<count;++i) {
        unsigned pin = requests[i].pb_pin;
        if (pin < 4 || pin > 8 || (pin != 4 && pin != 5 && pin != 6 && pin != data_pin) || (mask & BIT(pin)) ||
            requests[i].desired.function != 3 ||
            !a733_pin_config_valid(pin,requests[i].expected) ||
            !a733_pin_config_valid(pin,requests[i].desired)) return 0;
        mask |= BIT(pin);
    }
    if (mask != (BIT(5)|BIT(6)|BIT(data_pin)|(count == 4 ? BIT(4) : 0))) return 0;
    s->parent=parent; s->pins=pins; s->owner=owner; s->count=count;
    for (size_t i=0;i<count;++i) s->requests[i]=requests[i];
    s->route_mask=mask;s->initialized=1;
    return 1;
}
int a733_i2s_platform_init(struct a733_i2s_platform *s,struct a733_audio_parent *parent,
    struct a733_pins *pins,uint64_t owner,const struct a733_pin_request *requests,size_t count)
{return init_route(s,parent,pins,owner,requests,count,8);}
static enum a733_i2s_resource_result local_poll(void *cookie, uint64_t deadline)
{
    struct a733_i2s_platform *s=cookie;
    if (!s || !s->initialized || s->failed || !a733_audio_parent_ready(s->parent)) return RETAINED;
    struct a733_audio_parent *p=s->parent;
    if (deadline != s->deadline || !time_ok(p,deadline)) return local_fail(s);
    if (s->stage == L_READY) return READY;
    if (s->stage == L_LEASE) {
        enum a733_i2s_resource_result r=lease(p,A733_AUDIO_LOCAL,&s->local,deadline);
        if (r == RETAINED) return local_fail(s);
        if (r != READY) return PENDING;
        s->stage=L_CHECK;
        return PENDING;
    }
    if (s->stage == L_PINS) {
        enum a733_pins_result r=a733_pins_acquire(s->pins,s->owner,s->requests,s->count,
                                                &s->pin_lease,deadline);
        if (r == A733_PINS_RECOVERY || !time_ok(p,deadline)) return local_fail(s);
        if (r == A733_PINS_BUSY || r == A733_PINS_CONFLICT) return PENDING;
        if (r != A733_PINS_OK) return local_fail(s);
        s->stage=L_RESET_WAIT; s->wait_since=p->last_time;
        return PENDING;
    }
    if (s->stage == L_RESET_WAIT) {
        if (p->last_time - s->wait_since < 10) return PENDING;
        s->stage=L_ENABLE;
    }
    int held=enter(p,deadline);
    if (held < 0) return local_fail(s);
    if (!held) return PENDING;
    int ok=1;
    if (s->stage == L_CHECK) {
        ok=rd(p,deadline,MODULE,&s->initial_module) && !(s->initial_module & BIT(31)) &&
           rd(p,deadline,BUS,&s->initial_bus) && !(s->initial_bus & BUS_MASK);
        if (ok) s->stage=L_PINS;
    } else if (s->stage == L_ENABLE) {
        /* The reset line has remained asserted for at least 10 us. */
        ok=update(p,deadline,MODULE,MODULE_MASK,MODULE_VALUE) &&
           update(p,deadline,BUS,BIT(16),BIT(16)) &&
           update(p,deadline,BUS,BIT(0),BIT(0)) &&
           update(p,deadline,MODULE,BIT(31),BIT(31));
        if (ok) s->stage=L_READY;
    } else ok=0;
    if(s->stream_mode && (!ok || !time_ok(p,deadline))) {
        s->register_retained=1;return local_fail(s);
    }
    p->ops.leave(p->cookie);
    if (!ok || !time_ok(p,deadline)) return local_fail(s);
    return s->stage == L_READY ? READY : PENDING;
}
static enum a733_i2s_resource_result local_acquire(void *cookie,
    const struct a733_i2s_plan *plan, uint64_t deadline)
{
    struct a733_i2s_platform *s=cookie;
    if (!s || !s->initialized || s->failed) return RETAINED;
    if (s->stage != L_IDLE || !plan || !a733_audio_parent_ready(s->parent) ||
        plan->format.module_hz != 24576000 || plan->format.rate_hz != 16000 ||
        plan->format.slot_bits != 32 || (plan->format.mclk_hz && plan->format.mclk_hz != 12288000) ||
        (plan->format.mclk_hz != 0) != (s->count == 4) || plan->pb_mask!=s->route_mask) return BUSY;
    if (!time_ok(s->parent,deadline)) return BUSY;
    s->plan=*plan; s->deadline=deadline; s->stage=L_LEASE;
    /* OPENING owns this context even when the common lease service is busy. */
    return local_poll(s,deadline);
}
static enum a733_i2s_resource_result local_release(void *cookie, uint64_t deadline)
{
    struct a733_i2s_platform *s=cookie;
    if (!s || !s->initialized || s->failed || !a733_audio_parent_ready(s->parent)) return RETAINED;
    struct a733_audio_parent *p=s->parent;
    if (!time_ok(p,deadline)) return local_fail(s);
    if (s->stage == L_READY) { s->stage=L_STOP; s->deadline=deadline; }
    if (deadline != s->deadline) return local_fail(s);
    if (s->stage == L_STOP) {
        int held=enter(p,deadline);
        if (held < 0) return local_fail(s);
        if (!held) return PENDING;
        int ok=update(p,deadline,MODULE,BIT(31),0) &&
            update(p,deadline,BUS,BIT(16),0) && update(p,deadline,BUS,BIT(0),0) &&
            update(p,deadline,MODULE,MODULE_MASK,s->initial_module) &&
            update(p,deadline,BUS,BUS_MASK,s->initial_bus);
        if(s->stream_mode && (!ok || !time_ok(p,deadline))) {
            s->register_retained=1;return local_fail(s);
        }
        p->ops.leave(p->cookie);
        if (!ok || !time_ok(p,deadline)) return local_fail(s);
        s->stage=L_UNPIN;
        return PENDING;
    }
    if (s->stage == L_UNPIN) {
        struct a733_pin_lease saved=s->pin_lease;
        enum a733_pins_result r=a733_pins_release(s->pins,&s->pin_lease,1,deadline);
        if (r == A733_PINS_RECOVERY || !time_ok(p,deadline)) {
            if (!s->pin_lease.id) s->pin_lease=saved; /* Conservative identity after late return. */
            return local_fail(s);
        }
        if (r == A733_PINS_BUSY) return PENDING;
        if (r != A733_PINS_OK) return local_fail(s);
        s->stage=L_RELEASE;
        return PENDING;
    }
    if (s->stage != L_RELEASE) return local_fail(s);
    enum a733_i2s_resource_result r=p->ops.release(p->cookie,&s->local,deadline);
    if (r == RETAINED || !time_ok(p,deadline)) return local_fail(s);
    if (r == BUSY || r == PENDING) return PENDING;
    if (r != READY) return local_fail(s);
    s->local=(struct a733_audio_token){0}; s->stage=L_IDLE;
    return READY;
}
static int local_now(void *cookie, uint64_t *now)
{
    struct a733_i2s_platform *s=cookie;
    if (!s || !s->initialized || s->failed || !a733_audio_parent_ready(s->parent)) return 0;
    uint64_t observed;
    if (!now || !s->parent->ops.now_us(s->parent->cookie,&observed) ||
        observed < s->parent->last_time) {
        s->failed=1;
        return 0;
    }
    s->parent->last_time=observed;
    *now=observed;
    return 1;
}
static int local_read(void *cookie, uintptr_t a, uint32_t *v)
{
    struct a733_i2s_platform *s=cookie;
    if (!s || s->failed || s->stage != L_READY || !a733_audio_parent_ready(s->parent)) return 0;
    if (s->parent->ops.read(s->parent->cookie,a,v)) return 1;
    s->failed=1; return 0;
}
static int local_write(void *cookie, uintptr_t a, uint32_t v)
{
    struct a733_i2s_platform *s=cookie;
    if (!s || s->failed || s->stage != L_READY || !a733_audio_parent_ready(s->parent)) return 0;
    if (s->parent->ops.write(s->parent->cookie,a,v)) return 1;
    s->failed=1; return 0;
}
struct a733_i2s_operations a733_i2s_platform_operations(void)
{
    return (struct a733_i2s_operations){local_acquire,local_poll,local_release,
                                       local_now,local_read,local_write};
}

static int lease_operations_match(const struct a733_audio_parent *p,const struct a733_audio_leases *b)
{
    return p->cookie==b && p->ops.acquire==b->operations.acquire &&
        p->ops.publish==b->operations.publish && p->ops.release==b->operations.release &&
        p->ops.enter==b->operations.enter && p->ops.leave==b->operations.leave &&
        p->ops.now_us==b->operations.now_us && p->ops.read==b->operations.read &&
        p->ops.write==b->operations.write;
}
int a733_i2s_platform_stream_init(struct a733_i2s_platform *s,struct a733_audio_parent *p,
    struct a733_pins *pins,struct a733_audio_leases *leases,uint64_t owner,
    const struct a733_pin_request *requests,size_t count,enum a733_i2s_direction direction)
{
    if(!p || !leases || !leases->initialized || leases->failed || leases->owner!=owner ||
        !lease_operations_match(p,leases) || (unsigned)direction>A733_I2S_TRANSMIT ||
        !init_route(s,p,pins,owner,requests,count,direction==A733_I2S_TRANSMIT?7:8))return 0;
    s->stream_leases=leases;s->stream_mode=1;return 1;
}
static enum a733_i2s_resource_result stream_authority(void *cookie,uint64_t deadline,int ready)
{
    struct a733_i2s_platform *s=cookie;
    if(!s || !s->initialized || !s->stream_mode || s->failed || s->register_retained ||
        (ready && s->stage!=L_READY) || !a733_audio_parent_ready(s->parent))return RETAINED;
    struct a733_audio_leases *b=s->stream_leases;
    if(!b || !b->initialized || b->failed || b->owner!=s->owner ||
        !lease_operations_match(s->parent,b) || s->pins->failed ||
        !time_ok(s->parent,deadline))return local_fail(s);
    const struct a733_audio_token actual[]={s->parent->reference,s->parent->audio,s->local};
    static const enum a733_audio_scope scopes[]={A733_AUDIO_REFERENCE,A733_AUDIO_STABLE,A733_AUDIO_LOCAL};
    static const a733_clock_mask_t shared[]={A733_CLOCK_REF|A733_CLOCK_OSC,
        A733_CLOCK_AUDIO5|A733_CLOCK_AUDIO1|A733_CLOCK_REF|A733_CLOCK_OSC,0};
    for(unsigned i=0;i<(s->local.id?3u:2u);++i) {
        const struct a733_clock_token *t=&b->tokens[scopes[i]-1];
        if(actual[i].scope!=scopes[i] || !actual[i].id || actual[i].id!=t->id ||
            t->owner!=s->owner || t->shared!=shared[i] ||
            t->exclusive!=(i==2?A733_CLOCK_I2S0:0))return local_fail(s);
        enum a733_clock_result r=a733_clock_token_status(b->registry,t);
        if(!time_ok(s->parent,deadline))return local_fail(s);
        if(r==A733_CLOCK_BUSY)return BUSY;
        if(r!=A733_CLOCK_OK)return local_fail(s);
    }
    if(!s->local.id && (ready || b->tokens[A733_AUDIO_LOCAL-1].id))return local_fail(s);
    if(!s->pin_lease.id) {
        if(ready)return local_fail(s);
        s->stream_deadline=deadline;return READY;
    }
    if(s->pin_lease.owner!=s->owner || s->pin_lease.pb_mask!=s->route_mask)
        return local_fail(s);
    int held=0;
    enum a733_pins_result r=a733_pins_held(s->pins,&s->pin_lease,&held,deadline);
    if(!time_ok(s->parent,deadline))return local_fail(s);
    if(r==A733_PINS_BUSY)return BUSY;
    if(r!=A733_PINS_OK || !held || s->pins->failed)return local_fail(s);
    s->stream_deadline=deadline;return READY;
}
static enum a733_i2s_resource_result stream_health(void *cookie,uint64_t deadline)
{return stream_authority(cookie,deadline,1);}
static enum a733_i2s_resource_result stream_acquire(void *cookie,const struct a733_i2s_plan *plan,uint64_t deadline)
{
    enum a733_i2s_resource_result r=stream_authority(cookie,deadline,0);
    return r==READY?local_acquire(cookie,plan,deadline):r;
}
static enum a733_i2s_resource_result stream_acquire_poll(void *cookie,uint64_t deadline)
{
    enum a733_i2s_resource_result r=stream_authority(cookie,deadline,0);
    return r==READY?local_poll(cookie,deadline):r;
}
static enum a733_i2s_resource_result stream_release(void *cookie,uint64_t deadline)
{
    enum a733_i2s_resource_result r=stream_authority(cookie,deadline,0);
    return r==READY?local_release(cookie,deadline):r;
}
static int stream_register(struct a733_i2s_platform *s,uintptr_t a,uint32_t *value,int writing)
{
    if(!s || s->failed || !s->stream_mode || s->register_retained || s->stage!=L_READY ||
        !a733_audio_parent_ready(s->parent) || !s->stream_deadline ||
        a<A733_I2S0_BASE || a>A733_I2S0_BASE+0x74 || (a&3))return 0;
    struct a733_audio_parent *p=s->parent;
    if(!time_ok(p,s->stream_deadline)){s->failed=1;return 0;}
    /* Live-lease queries have completed before entering this mutex. */
    if(!p->ops.enter(p->cookie,s->stream_deadline)){s->failed=1;return 0;}
    if(!time_ok(p,s->stream_deadline)) {
        p->ops.leave(p->cookie);s->failed=1;return 0;
    }
    uint32_t observed=0;
    int ok=writing?p->ops.write(p->cookie,a,*value):p->ops.read(p->cookie,a,&observed);
    if(ok && writing && a!=A733_I2S0_BASE+12) {
        uint32_t got;
        ok=time_ok(p,s->stream_deadline) && p->ops.read(p->cookie,a,&got);
        /* FIFO flush bits self-clear; all other written fields must match. */
        uint32_t ignore=a==A733_I2S0_BASE+20 ? (3u<<24):0;
        if(ok)ok=(got&~ignore)==(*value&~ignore);
    }
    if(!ok || !time_ok(p,s->stream_deadline)) {
        s->register_retained=1;s->failed=1;return 0;
    }
    p->ops.leave(p->cookie);
    if(!time_ok(p,s->stream_deadline)){s->failed=1;return 0;}
    if(!writing)*value=observed;
    return 1;
}
static int stream_read(void *cookie,uintptr_t a,uint32_t *value)
{return value && stream_register(cookie,a,value,0);}
static int stream_write(void *cookie,uintptr_t a,uint32_t value)
{return stream_register(cookie,a,&value,1);}
struct a733_i2s_stream_operations a733_i2s_platform_stream_operations(void)
{
    return (struct a733_i2s_stream_operations){
        {stream_acquire,stream_acquire_poll,stream_release,local_now,stream_read,stream_write},stream_health};
}
