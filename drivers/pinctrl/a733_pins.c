#include "a733_pins.h"

/* Pinned A733 types 4 and 10 share these mux/pull/drive/IRQ addresses. */
enum { MUX0=0x100, MUX1=0x104, DRIVE0=0x120, DRIVE1=0x124, PULL=0x130,
       DATA=0x110, IRQ_ENABLE=0x150, WORDS=5 };
static const unsigned offsets[WORDS] = {MUX0, MUX1, DRIVE0, DRIVE1, PULL};
struct plan { uint32_t mask[WORDS], old[WORDS], target[WORDS]; };

int a733_pin_fields(unsigned pin, struct a733_pin_fields *out)
{
    if (!out || pin < 4 || pin > 8) return 0;
    *out = (struct a733_pin_fields){A733_PINS_BASE + MUX0 + (pin/8)*4,
        A733_PINS_BASE + PULL, A733_PINS_BASE + DRIVE0 + (pin/8)*4,
        (pin%8)*4, pin*2, (pin%8)*4};
    return 1;
}
int a733_pin_config_valid(unsigned pin, struct a733_pin_config c)
{
    /* Function 1 is GPIO output and 14 is EINT: both require further services. */
    static const uint16_t functions[A733_PINS_COUNT] = {
        0x81fdu, 0x81f9u, 0x80fdu, 0x81fdu, 0x81fdu
    };
    return pin >= 4 && pin <= 8 && c.function < 16 &&
           (functions[pin-4] & (1u << c.function)) && c.pull <= 2 && c.drive <= 3;
}
int a733_pins_init(struct a733_pins *m, const struct a733_pins_operations *ops,
                    void *cookie, uint32_t authorized)
{
    if (!authorized || (authorized & ~A733_PINS_PB_MASK)) return 0;
    struct a733_pin_handoff handoff={0};
    handoff.authorized[A733_PIN_B]=authorized;
    return a733_pins_init_banks(m,ops,cookie,&handoff);
}
static int in_time(struct a733_pins *m, uint64_t deadline)
{
    uint64_t now;
    if (!m->operations.now(m->cookie, &now) || now < m->last_time) return 0;
    m->last_time = now;
    return now < deadline;
}
static enum a733_pins_result enter(struct a733_pins *m, uint64_t deadline)
{
    if (!m || !m->initialized) return A733_PINS_INVALID;
    uint64_t before;
    if (!m->operations.now(m->cookie, &before) || before >= deadline ||
        !m->operations.enter(m->cookie, deadline)) return A733_PINS_BUSY;
    /* last_time belongs to the shared lock. Another operation may have run
     * between our first clock read and admission. */
    if (before > m->last_time) m->last_time = before;
    if (!in_time(m, deadline)) { m->operations.leave(m->cookie); return A733_PINS_BUSY; }
    return A733_PINS_OK;
}
static enum a733_pins_result leave(struct a733_pins *m, enum a733_pins_result result)
{ m->operations.leave(m->cookie); return result; }
static enum a733_pins_result fault(struct a733_pins *m)
{ m->failed = 1; return A733_PINS_RECOVERY; }
static int read_word(struct a733_pins *m, unsigned offset, uint32_t *v)
{ return m->operations.read(m->cookie, A733_PINS_BASE + offset, v); }
static int match(struct a733_pins *m, const struct a733_pin_lease *h)
{
    if (!h || !h->id || !h->owner || !h->pb_mask || (h->pb_mask & ~m->authorized_pb_mask)) return 0;
    uint32_t found = 0;
    for (unsigned bank=1; bank<A733_PIN_BANKS; ++bank)
        for (unsigned pin=0; pin<32; ++pin)
            if (m->pins[bank][pin].id==h->id && m->pins[bank][pin].owner==h->owner) {
                if (bank!=A733_PIN_B || pin<4 || pin>8 || m->pins[bank][pin].banked) return 0;
                found |= 1u<<pin;
            }
    return found == h->pb_mask;
}
static void add_fields(struct plan *p, unsigned pin, struct a733_pin_config initial,
                       struct a733_pin_config target)
{
    unsigned word = pin/8, shift = (pin%8)*4;
    p->mask[word] |= 15u<<shift; p->old[word] |= (uint32_t)initial.function<<shift;
    p->target[word] |= (uint32_t)target.function<<shift;
    p->mask[word+2] |= 15u<<shift; p->old[word+2] |= (uint32_t)initial.drive<<shift;
    p->target[word+2] |= (uint32_t)target.drive<<shift;
    p->mask[4] |= 3u<<(pin*2); p->old[4] |= (uint32_t)initial.pull<<(pin*2);
    p->target[4] |= (uint32_t)target.pull<<(pin*2);
}
/* No writes. Matching bits establish configuration, never ownership by itself. */
static enum a733_pins_result preflight(struct a733_pins *m, const struct plan *p,
                                       uint32_t pins, uint64_t deadline)
{
    uint32_t value;
    if (!in_time(m, deadline)) return A733_PINS_BUSY;
    if (!read_word(m, IRQ_ENABLE, &value)) return fault(m);
    if (value & pins) return A733_PINS_CONFLICT;
    for (unsigned i=0; i<WORDS; ++i) if (p->mask[i]) {
        if (!in_time(m, deadline)) return A733_PINS_BUSY;
        if (!read_word(m, offsets[i], &value)) return fault(m);
        if ((value & p->mask[i]) != p->old[i]) return A733_PINS_CONFLICT;
    }
    return A733_PINS_OK;
}
/* The mutex covers the full read-modify-write-readback sequence and all words.
 * A failed write may already have reached hardware; no compensating I/O follows. */
static enum a733_pins_result update(struct a733_pins *m, unsigned word, uint32_t mask, uint32_t expected,
                   uint32_t bits, uint64_t deadline, unsigned *attempted)
{
    uint32_t before, after;
    if (!in_time(m, deadline)) return *attempted ? A733_PINS_RECOVERY : A733_PINS_BUSY;
    if (!read_word(m, offsets[word], &before) || (before & mask) != expected)
        return A733_PINS_RECOVERY;
    uint32_t value = (before & ~mask) | bits;
    if (before == value) {
        if (!in_time(m, deadline)) return *attempted ? A733_PINS_RECOVERY : A733_PINS_BUSY;
        return A733_PINS_OK;
    }
    if (!in_time(m, deadline)) return *attempted ? A733_PINS_RECOVERY : A733_PINS_BUSY;
    *attempted = 1;
    if (!m->operations.write(m->cookie, A733_PINS_BASE+offsets[word], value) ||
        !read_word(m, offsets[word], &after) || after != value) return A733_PINS_RECOVERY;
    if (!in_time(m, deadline)) return A733_PINS_RECOVERY;
    return A733_PINS_OK;
}
static enum a733_pins_result apply(struct a733_pins *m, const struct plan *p,
                                    uint64_t deadline, unsigned *attempted)
{
    enum a733_pins_result result;
    for (unsigned i=0; i<2; ++i) if (p->mask[i]) {
        result=update(m,i,p->mask[i],p->old[i],p->mask[i],deadline,attempted);
        if (result!=A733_PINS_OK) return result;
    }
    for (unsigned i=2; i<WORDS; ++i) if (p->mask[i]) {
        result=update(m,i,p->mask[i],p->old[i],p->target[i],deadline,attempted);
        if (result!=A733_PINS_OK) return result;
    }
    for (unsigned i=0; i<2; ++i) if (p->mask[i]) {
        result=update(m,i,p->mask[i],p->mask[i],p->target[i],deadline,attempted);
        if (result!=A733_PINS_OK) return result;
    }
    return A733_PINS_OK;
}
/* Output aliases could overwrite manager ownership or the operation's input. */
static int overlap(const void *a, size_t n, const void *b, size_t k)
{
    uintptr_t x=(uintptr_t)a, y=(uintptr_t)b;
    return n>UINTPTR_MAX-x || k>UINTPTR_MAX-y || (x<y+k && y<x+n);
}
enum a733_pins_result a733_pins_acquire(struct a733_pins *m, uint64_t owner,
    const struct a733_pin_request *r, size_t count, struct a733_pin_lease *out, uint64_t deadline)
{
    if (!m || !r || !out || !owner || !count || count>A733_PINS_COUNT ||
        out->id || out->owner || out->pb_mask ||
        overlap(m,sizeof(*m),out,sizeof(*out)) || overlap(m,sizeof(*m),r,count*sizeof(*r)) ||
        overlap(out,sizeof(*out),r,count*sizeof(*r))) return A733_PINS_INVALID;
    struct plan p={0}; uint32_t mask=0;
    for (size_t i=0; i<count; ++i) {
        unsigned pin=r[i].pb_pin;
        if (!a733_pin_config_valid(pin,r[i].expected) || !a733_pin_config_valid(pin,r[i].desired) ||
            (mask & (1u<<pin))) return A733_PINS_INVALID;
        mask |= 1u<<pin; add_fields(&p,pin,r[i].expected,r[i].desired);
    }
    enum a733_pins_result result=enter(m,deadline);
    if (result!=A733_PINS_OK) return result;
    if (m->failed) return leave(m,A733_PINS_RECOVERY);
    if ((mask & ~m->authorized_pb_mask) || !m->next_id) return leave(m,A733_PINS_INVALID);
    for (unsigned i=0; i<A733_PINS_COUNT; ++i)
        if ((mask & (1u<<(i+4))) && m->pins[A733_PIN_B][i+4].id) return leave(m,A733_PINS_CONFLICT);
    result=preflight(m,&p,mask,deadline);
    if (result!=A733_PINS_OK) return leave(m,result);
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    *out=(struct a733_pin_lease){m->next_id++,owner,mask};
    for (size_t i=0; i<count; ++i)
        m->pins[A733_PIN_B][r[i].pb_pin]=(struct a733_pin_state){.id=out->id,.owner=owner,.initial=r[i].expected,.current=r[i].desired};
    unsigned attempted=0;
    result=apply(m,&p,deadline,&attempted);
    if (result==A733_PINS_BUSY) {
        for (unsigned i=0; i<A733_PINS_COUNT; ++i)
            if (mask & (1u<<(i+4))) m->pins[A733_PIN_B][i+4]=(struct a733_pin_state){0};
        *out=(struct a733_pin_lease){0};
        return leave(m,result);
    }
    if (result!=A733_PINS_OK) return leave(m,fault(m));
    return leave(m,A733_PINS_OK);
}
enum a733_pins_result a733_pins_release(struct a733_pins *m, struct a733_pin_lease *h,
                                        unsigned quiescent, uint64_t deadline)
{
    if (!m || !h || quiescent!=1 || overlap(m,sizeof(*m),h,sizeof(*h))) return A733_PINS_INVALID;
    enum a733_pins_result result=enter(m,deadline);
    if (result!=A733_PINS_OK) return result;
    if (!match(m,h)) return leave(m,A733_PINS_CONFLICT);
    if (m->failed) return leave(m,A733_PINS_RECOVERY);
    struct plan p={0};
    for (unsigned i=0; i<A733_PINS_COUNT; ++i) if (h->pb_mask & (1u<<(i+4)))
        add_fields(&p,i+4,m->pins[A733_PIN_B][i+4].current,m->pins[A733_PIN_B][i+4].initial);
    result=preflight(m,&p,h->pb_mask,deadline);
    if (result==A733_PINS_BUSY) return leave(m,result);
    if (result!=A733_PINS_OK) return leave(m,fault(m));
    unsigned attempted=0;
    result=apply(m,&p,deadline,&attempted);
    if (result==A733_PINS_BUSY) return leave(m,result);
    if (result!=A733_PINS_OK) return leave(m,fault(m));
    for (unsigned i=0; i<A733_PINS_COUNT; ++i)
        if (h->pb_mask & (1u<<(i+4))) m->pins[A733_PIN_B][i+4]=(struct a733_pin_state){0};
    *h=(struct a733_pin_lease){0};
    return leave(m,A733_PINS_OK);
}
enum a733_pins_result a733_pins_read_inputs(struct a733_pins *m, const struct a733_pin_lease *h,
    uint32_t mask, uint32_t *levels, uint64_t deadline)
{
    if (!m || !h || !levels || !mask || (mask & ~h->pb_mask) ||
        overlap(m,sizeof(*m),levels,sizeof(*levels)) || overlap(h,sizeof(*h),levels,sizeof(*levels)))
        return A733_PINS_INVALID;
    enum a733_pins_result result=enter(m,deadline);
    if (result!=A733_PINS_OK) return result;
    if (!match(m,h)) return leave(m,A733_PINS_CONFLICT);
    if (m->failed) return leave(m,A733_PINS_RECOVERY);
    struct plan p={0};
    for (unsigned i=0; i<A733_PINS_COUNT; ++i) if (mask & (1u<<(i+4))) {
        if (m->pins[A733_PIN_B][i+4].current.function!=0) return leave(m,A733_PINS_INVALID);
        add_fields(&p,i+4,m->pins[A733_PIN_B][i+4].current,m->pins[A733_PIN_B][i+4].current);
    }
    result=preflight(m,&p,mask,deadline);
    if (result==A733_PINS_BUSY) return leave(m,result);
    if (result!=A733_PINS_OK) return leave(m,fault(m));
    uint32_t value;
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    if (!read_word(m,DATA,&value)) return leave(m,fault(m));
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    *levels=value & mask;
    return leave(m,A733_PINS_OK);
}
enum a733_pins_result a733_pins_held(struct a733_pins *m, const struct a733_pin_lease *h,
                                   int *held, uint64_t deadline)
{
    if (!m || !h || !held || overlap(m,sizeof(*m),held,sizeof(*held)) ||
        overlap(h,sizeof(*h),held,sizeof(*held))) return A733_PINS_INVALID;
    enum a733_pins_result result=enter(m,deadline);
    if (result!=A733_PINS_OK) return result;
    *held=match(m,h);
    return leave(m,A733_PINS_OK);
}

/* Expanded operations use the same state and mutex as the PB entry points. */
#define DEBOUNCE_MASK UINT32_C(0x71)
static uintptr_t bank_base(unsigned b)
{ return b<A733_PIN_L ? A733_PINS_BASE+0x80+b*0x80 : A733_PINS_R_BASE+(b-A733_PIN_L)*0x30; }
static uintptr_t irq_base(unsigned b)
{ return b<A733_PIN_L ? bank_base(b)+0x40 : A733_PINS_R_BASE+0x200+(b-A733_PIN_L)*0x20; }
static int rd(struct a733_pins *m, uintptr_t a, uint32_t *v)
{ return m->operations.read(m->cookie,a,v); }
static int empty_banked(const struct a733_pin_lease_banked *h)
{
    if (h->id || h->owner) return 0;
    for (unsigned b=0;b<A733_PIN_BANKS;++b) if (h->masks[b]) return 0;
    return 1;
}
int a733_pins_init_banks(struct a733_pins *m,const struct a733_pins_operations *ops,
    void *cookie,const struct a733_pin_handoff *h)
{
    if (!m || m->initialized || !ops || !ops->enter || !ops->leave || !ops->now ||
        !ops->read || !ops->write || !h || (h->irq_banks & ~UINT32_C(0x1ffe)) ||
        overlap(m,sizeof(*m),h,sizeof(*h))) return 0;
    uint32_t any=0;
    for (unsigned b=0;b<A733_PIN_BANKS;++b) {
        if (h->authorized[b] & ~a733_pin_bank_mask(b)) return 0;
        any |= h->authorized[b];
    }
    if (!any) return 0;
    for (unsigned i=0;i<2;++i) {
        const struct a733_pin_latch_handoff *l=&h->latch[i];
        if (!l->identity && !l->owner && !l->known_mask && !l->value) continue;
        uint32_t valid=a733_pin_bank_mask(A733_PIN_L+i);
        if (!l->identity || !l->owner || l->known_mask!=valid || (l->value & ~valid)) return 0;
    }
    /* The caller supplies zero-initialized private storage, as for the PB API. */
    m->operations=*ops; m->cookie=cookie; m->handoff=*h;
    m->authorized_pb_mask=h->authorized[A733_PIN_B]&A733_PINS_PB_MASK;
    for (unsigned i=0;i<2;++i) m->banks[A733_PIN_L+i].latch=h->latch[i].value;
    m->next_id=1; m->initialized=1;
    return 1;
}
static int match_banked(struct a733_pins *m,const struct a733_pin_lease_banked *h)
{
    if (!h || !h->id || !h->owner || h->masks[0]) return 0;
    uint32_t any=0;
    for (unsigned b=1;b<A733_PIN_BANKS;++b) {
        uint32_t found=0;
        for (unsigned p=0;p<32;++p)
            if (m->pins[b][p].id==h->id && m->pins[b][p].owner==h->owner) {
                if (!m->pins[b][p].banked) return 0;
                found|=1u<<p;
            }
        if (found!=h->masks[b] || (found & ~m->handoff.authorized[b])) return 0;
        any|=found;
    }
    return any!=0;
}
static enum a733_pins_result field_match(struct a733_pins *m,uintptr_t a,
    unsigned shift,uint32_t mask,uint32_t expected,uint64_t deadline)
{
    uint32_t v;
    if (!in_time(m,deadline)) return A733_PINS_BUSY;
    if (!rd(m,a,&v)) return fault(m);
    return ((v>>shift)&mask)==expected ? A733_PINS_OK : A733_PINS_CONFLICT;
}
static enum a733_pins_result config_match(struct a733_pins *m,unsigned b,unsigned p,
    struct a733_pin_config c,uint64_t deadline)
{
    struct a733_pin_fields f;
    if (!a733_pin_fields_banked(b,p,&f)) return A733_PINS_INVALID;
    enum a733_pins_result r=field_match(m,f.mux_address,f.mux_shift,15,c.function,deadline);
    if (r==A733_PINS_OK) r=field_match(m,f.drive_address,f.drive_shift,15,c.drive,deadline);
    if (r==A733_PINS_OK) r=field_match(m,f.pull_address,f.pull_shift,3,c.pull,deadline);
    return r;
}
/* All changed words preserve their unowned fields. A failed or late write
 * retains the complete lease; no restoration follows uncertainty. */
static enum a733_pins_result rmw(struct a733_pins *m,uintptr_t a,uint32_t mask,
    uint32_t expected,uint32_t bits,uint64_t deadline,unsigned *attempted)
{
    uint32_t before,after;
    if (!in_time(m,deadline)) return *attempted ? A733_PINS_RECOVERY : A733_PINS_BUSY;
    if (!rd(m,a,&before) || (before&mask)!=expected) return A733_PINS_RECOVERY;
    uint32_t value=(before&~mask)|bits;
    if (!in_time(m,deadline)) return *attempted ? A733_PINS_RECOVERY : A733_PINS_BUSY;
    if (value==before) return A733_PINS_OK;
    *attempted=1;
    if (!m->operations.write(m->cookie,a,value) || !rd(m,a,&after) || after!=value ||
        !in_time(m,deadline)) return A733_PINS_RECOVERY;
    return A733_PINS_OK;
}
static enum a733_pins_result put(struct a733_pins *m,uintptr_t a,uint32_t bits,
    uint64_t deadline,unsigned *attempted)
{
    if (!in_time(m,deadline)) return *attempted ? A733_PINS_RECOVERY : A733_PINS_BUSY;
    *attempted=1;
    if (!m->operations.write(m->cookie,a,bits) || !in_time(m,deadline)) return A733_PINS_RECOVERY;
    return A733_PINS_OK;
}
/* Only GPIO outputs have meaningful latch readback. Input and alternate DATA
 * readings cannot initialize or repair the always-on bank shadow. */
static enum a733_pins_result latch_consistent(struct a733_pins *m,unsigned b,uint64_t deadline)
{
    uint32_t outputs=0,v,valid=a733_pin_bank_mask(b);
    for (unsigned group=0;group<4;++group) if (valid&(UINT32_C(0xff)<<(group*8))) {
        if (!in_time(m,deadline)) return A733_PINS_BUSY;
        if (!rd(m,bank_base(b)+group*4,&v)) return A733_PINS_RECOVERY;
        for (unsigned i=0;i<8;++i)
            if (((v>>(i*4))&15)==1) outputs|=(1u<<(group*8+i))&valid;
    }
    if (!in_time(m,deadline)) return A733_PINS_BUSY;
    if (!rd(m,bank_base(b)+0x10,&v)) return A733_PINS_RECOVERY;
    if ((v&outputs)!=(m->banks[b].latch&outputs)) return A733_PINS_RECOVERY;
    return in_time(m,deadline) ? A733_PINS_OK : A733_PINS_BUSY;
}
static enum a733_pins_result output_latch(struct a733_pins *m,unsigned b,uint32_t mask,
    uint32_t levels,uint64_t deadline,unsigned *attempted)
{
    enum a733_pins_result r;
    if (b<A733_PIN_L) {
        /* Clear and set only owned bits. The final mux selection follows both. */
        if (mask&~levels) {
            r=put(m,bank_base(b)+0x18,mask&~levels,deadline,attempted);
            if (r!=A733_PINS_OK) return r;
        }
        if (levels) return put(m,bank_base(b)+0x14,levels,deadline,attempted);
        return A733_PINS_OK;
    }
    if (!m->handoff.latch[b-A733_PIN_L].identity) return A733_PINS_INVALID;
    r=latch_consistent(m,b,deadline);
    if (r!=A733_PINS_OK) return *attempted ? A733_PINS_RECOVERY : r;
    uint32_t value=(m->banks[b].latch&~mask)|levels;
    r=put(m,bank_base(b)+0x10,value,deadline,attempted);
    /* Preserve the intended full-bank value even if the write is uncertain. */
    if (*attempted) m->banks[b].latch=value;
    return r;
}
static uint16_t uart_routes(unsigned b,unsigned p,struct a733_pin_config c)
{
    unsigned route=a733_pin_descriptor(b,p)->uart[c.function];
    return route ? (uint16_t)(1u<<route) : 0;
}
/* Route sets come from the manual's signal assignments. Pin adjacency does
 * not establish a route: PF2/PF4 are a pair, while PM2..PM5 contain two pairs. */
static int route_group(unsigned groups[10],unsigned b,unsigned p,struct a733_pin_config c)
{
    const struct a733_pin_descriptor *d=a733_pin_descriptor(b,p);
    unsigned uart=d->uart[c.function],group=d->uart_group[c.function];
    if (!uart) return 1;
    if (!group || (groups[uart] && groups[uart]!=group)) return 0;
    groups[uart]=group;
    return 1;
}
static enum a733_pins_result verify_pin(struct a733_pins *m,unsigned b,unsigned p,uint64_t deadline)
{
    struct a733_pin_state *s=&m->pins[b][p];
    enum a733_pins_result r=config_match(m,b,p,s->current,deadline);
    if (r!=A733_PINS_OK) return r;
    r=field_match(m,irq_base(b)+0x10,p,1,s->irq_enabled,deadline);
    if (r!=A733_PINS_OK) return r;
    if (s->current.function==14) {
        r=field_match(m,irq_base(b)+(p/8)*4,(p%8)*4,15,s->current_irq,deadline);
        if (r==A733_PINS_OK) r=field_match(m,irq_base(b)+0x18,0,DEBOUNCE_MASK,m->banks[b].debounce,deadline);
    } else if (s->current.function==1) {
        r=field_match(m,bank_base(b)+0x10,p,1,s->current_output,deadline);
    }
    return r;
}
static enum a733_pins_result checked(struct a733_pins *m,enum a733_pins_result r)
{ return r==A733_PINS_OK || r==A733_PINS_BUSY ? r : fault(m); }
/* Plan records are private to the call and carry everything needed for restore. */
struct expanded_plan {
    unsigned b,p;
    struct a733_pin_config old,target;
    uint8_t output,old_irq,irq;
};
static enum a733_pins_result apply_expanded(struct a733_pins *m,const struct expanded_plan *plan,
    size_t count,const uint32_t *deb_old,const uint32_t *deb_new,uint32_t deb_banks,
    uint64_t deadline,unsigned *attempted)
{
    enum a733_pins_result r;
    for (size_t i=0;i<count;++i) {
        const struct expanded_plan *q=&plan[i]; struct a733_pin_fields f;
        a733_pin_fields_banked(q->b,q->p,&f);
        r=rmw(m,f.mux_address,15u<<f.mux_shift,(uint32_t)q->old.function<<f.mux_shift,
              15u<<f.mux_shift,deadline,attempted);
        if (r!=A733_PINS_OK) return r;
    }
    for (size_t i=0;i<count;++i) {
        const struct expanded_plan *q=&plan[i]; struct a733_pin_fields f;
        a733_pin_fields_banked(q->b,q->p,&f);
        r=rmw(m,f.drive_address,15u<<f.drive_shift,(uint32_t)q->old.drive<<f.drive_shift,
              (uint32_t)q->target.drive<<f.drive_shift,deadline,attempted);
        if (r!=A733_PINS_OK) return r;
        r=rmw(m,f.pull_address,3u<<f.pull_shift,(uint32_t)q->old.pull<<f.pull_shift,
              (uint32_t)q->target.pull<<f.pull_shift,deadline,attempted);
        if (r!=A733_PINS_OK) return r;
        if (q->old.function==14 || q->target.function==14) {
            unsigned shift=(q->p%8)*4;
            r=rmw(m,irq_base(q->b)+(q->p/8)*4,15u<<shift,(uint32_t)q->old_irq<<shift,
                  (uint32_t)q->irq<<shift,deadline,attempted);
            if (r!=A733_PINS_OK) return r;
        }
    }
    for (unsigned b=1;b<A733_PIN_BANKS;++b) if (deb_banks&(1u<<b)) {
        r=rmw(m,irq_base(b)+0x18,DEBOUNCE_MASK,deb_old[b],deb_new[b],deadline,attempted);
        if (r!=A733_PINS_OK) return r;
    }
    for (size_t i=0;i<count;++i) if (plan[i].target.function==1) {
        const struct expanded_plan *q=&plan[i];
        r=output_latch(m,q->b,1u<<q->p,(uint32_t)q->output<<q->p,deadline,attempted);
        if (r!=A733_PINS_OK) return r;
    }
    for (size_t i=0;i<count;++i) {
        const struct expanded_plan *q=&plan[i]; struct a733_pin_fields f;
        a733_pin_fields_banked(q->b,q->p,&f);
        r=rmw(m,f.mux_address,15u<<f.mux_shift,15u<<f.mux_shift,
              (uint32_t)q->target.function<<f.mux_shift,deadline,attempted);
        if (r!=A733_PINS_OK) return r;
        if (q->target.function==1) {
            r=field_match(m,bank_base(q->b)+0x10,q->p,1,q->output,deadline);
            if (r!=A733_PINS_OK) return *attempted ? A733_PINS_RECOVERY : r;
        }
    }
    return in_time(m,deadline) ? A733_PINS_OK : (*attempted ? A733_PINS_RECOVERY : A733_PINS_BUSY);
}
static enum a733_pins_result acquire_banked(struct a733_pins *m,uint64_t owner,
    const struct a733_pin_request_banked *r,size_t count,
    const struct a733_pin_output_condition *condition,
    struct a733_pin_lease_banked *out,uint64_t deadline)
{
    if (!m || !r || !out || !owner || !count || count>A733_PIN_MAX_REQUESTS || !empty_banked(out) ||
        overlap(m,sizeof(*m),r,count*sizeof(*r)) || overlap(m,sizeof(*m),out,sizeof(*out)) ||
        overlap(out,sizeof(*out),r,count*sizeof(*r))) return A733_PINS_INVALID;
    struct a733_pin_output_condition expected={0};
    if (condition) {
        if (overlap(condition,sizeof(*condition),m,sizeof(*m)) ||
            overlap(condition,sizeof(*condition),r,count*sizeof(*r)) ||
            overlap(condition,sizeof(*condition),out,sizeof(*out))) return A733_PINS_INVALID;
        expected=*condition;
        uint32_t any=0;
        for (unsigned b=0;b<A733_PIN_BANKS;++b) {
            if (expected.levels[b]&~expected.masks[b]) return A733_PINS_INVALID;
            any|=expected.masks[b];
        }
        if (!any) return A733_PINS_INVALID;
    }
    struct expanded_plan plan[A733_PIN_MAX_REQUESTS];
    uint32_t masks[A733_PIN_BANKS]={0},irqs[A733_PIN_BANKS]={0};
    uint32_t deb_old[A733_PIN_BANKS]={0},deb_new[A733_PIN_BANKS]={0},deb_banks=0;
    uint16_t routes=0;
    unsigned old_groups[10]={0},new_groups[10]={0};
    for (size_t i=0;i<count;++i) {
        unsigned b=r[i].bank,p=r[i].pin;
        if (!a733_pin_config_valid_banked(b,p,r[i].expected) ||
            !a733_pin_config_valid_banked(b,p,r[i].desired) || r[i].expected.function==14 ||
            (masks[b]&(1u<<p)) || r[i].output_value>1 ||
            (r[i].desired.function!=1 && r[i].output_value) ||
            (r[i].desired.function!=14 && (r[i].irq_mode || r[i].debounce)) ||
            r[i].irq_mode>4 || (r[i].debounce & ~DEBOUNCE_MASK)) return A733_PINS_INVALID;
        if (!route_group(old_groups,b,p,r[i].expected) || !route_group(new_groups,b,p,r[i].desired))
            return A733_PINS_INVALID;
        if ((expected.masks[b]&(1u<<p)) &&
            (r[i].expected.function!=1 || r[i].desired.function!=1)) return A733_PINS_INVALID;
        masks[b]|=1u<<p;
        routes|=uart_routes(b,p,r[i].expected)|uart_routes(b,p,r[i].desired);
        plan[i]=(struct expanded_plan){b,p,r[i].expected,r[i].desired,r[i].output_value,0,r[i].irq_mode};
        if (r[i].desired.function==14) {
            if (irqs[b] && deb_new[b]!=r[i].debounce) return A733_PINS_INVALID;
            irqs[b]|=1u<<p; deb_new[b]=r[i].debounce;
        }
    }
    for (unsigned b=0;b<A733_PIN_BANKS;++b)
        if (expected.masks[b]&~masks[b]) return A733_PINS_INVALID;
    enum a733_pins_result result=enter(m,deadline);
    if (result!=A733_PINS_OK) return result;
    if (m->failed) return leave(m,A733_PINS_RECOVERY);
    if (!m->next_id) return leave(m,A733_PINS_INVALID);
    for (unsigned b=1;b<A733_PIN_BANKS;++b) {
        if (masks[b]&~m->handoff.authorized[b]) return leave(m,A733_PINS_INVALID);
        if (irqs[b] && !(m->handoff.irq_banks&(1u<<b))) return leave(m,A733_PINS_INVALID);
        for (unsigned p=0;p<32;++p) if (m->pins[b][p].id) {
            struct a733_pin_state *s=&m->pins[b][p];
            if ((masks[b]&(1u<<p)) || (routes&(uart_routes(b,p,s->initial)|uart_routes(b,p,s->current))))
                return leave(m,A733_PINS_CONFLICT);
        }
        if (!masks[b]) continue;
        uint32_t enabled;
        if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
        if (!rd(m,irq_base(b)+0x10,&enabled)) return leave(m,fault(m));
        if (enabled&masks[b]) return leave(m,A733_PINS_CONFLICT);
        if (irqs[b]) {
            if (enabled&~m->banks[b].irq_pins) return leave(m,A733_PINS_CONFLICT);
            uint32_t pending,deb;
            if (!rd(m,irq_base(b)+0x14,&pending) || !rd(m,irq_base(b)+0x18,&deb)) return leave(m,fault(m));
            if (pending&irqs[b]) return leave(m,A733_PINS_CONFLICT);
            if (m->banks[b].irq_pins) {
                if ((deb&DEBOUNCE_MASK)!=m->banks[b].debounce) return leave(m,fault(m));
                if (deb_new[b]!=m->banks[b].debounce) return leave(m,A733_PINS_CONFLICT);
            } else { deb_old[b]=deb&DEBOUNCE_MASK; deb_banks|=1u<<b; }
        }
    }
    uint8_t old_output[A733_PIN_MAX_REQUESTS]={0};
    for (size_t i=0;i<count;++i) {
        struct expanded_plan *q=&plan[i];
        if (q->b>=A733_PIN_L && (q->old.function==1 || q->target.function==1) &&
            !m->handoff.latch[q->b-A733_PIN_L].identity) return leave(m,A733_PINS_INVALID);
        result=config_match(m,q->b,q->p,q->old,deadline);
        if (result!=A733_PINS_OK) return leave(m,result);
        uint32_t value;
        if (q->old.function==1) {
            if (!rd(m,bank_base(q->b)+0x10,&value)) return leave(m,fault(m));
            old_output[i]=(value>>q->p)&1;
            if (q->b>=A733_PIN_L && old_output[i]!=((m->banks[q->b].latch>>q->p)&1)) return leave(m,fault(m));
            if ((expected.masks[q->b]&(1u<<q->p)) &&
                old_output[i]!=((expected.levels[q->b]>>q->p)&1)) return leave(m,A733_PINS_CONFLICT);
        }
        if (q->target.function==14) {
            if (!rd(m,irq_base(q->b)+(q->p/8)*4,&value)) return leave(m,fault(m));
            q->old_irq=(value>>((q->p%8)*4))&15;
        }
    }
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    out->id=m->next_id++; out->owner=owner;
    for (unsigned b=1;b<A733_PIN_BANKS;++b) out->masks[b]=masks[b];
    for (size_t i=0;i<count;++i) {
        struct expanded_plan *q=&plan[i];
        m->pins[q->b][q->p]=(struct a733_pin_state){.id=out->id,.owner=owner,
            .initial=q->old,.current=q->target,.initial_output=old_output[i],.current_output=q->output,
            .initial_irq=q->old_irq,.current_irq=q->irq,.banked=1};
    }
    unsigned attempted=0;
    result=apply_expanded(m,plan,count,deb_old,deb_new,deb_banks,deadline,&attempted);
    if (result==A733_PINS_BUSY) {
        for (size_t i=0;i<count;++i) m->pins[plan[i].b][plan[i].p]=(struct a733_pin_state){0};
        *out=(struct a733_pin_lease_banked){0};
        return leave(m,result);
    }
    /* These reservations remain authoritative on uncertain configuration. */
    for (unsigned b=1;b<A733_PIN_BANKS;++b) if (irqs[b]) {
        if (deb_banks&(1u<<b)) { m->banks[b].initial_debounce=deb_old[b]; m->banks[b].debounce=deb_new[b]; }
        m->banks[b].irq_pins|=irqs[b];
    }
    return leave(m,result==A733_PINS_OK ? result : fault(m));
}
enum a733_pins_result a733_pins_acquire_banked(struct a733_pins *m,uint64_t owner,
    const struct a733_pin_request_banked *r,size_t count,struct a733_pin_lease_banked *out,uint64_t deadline)
{
    return acquire_banked(m,owner,r,count,0,out,deadline);
}
enum a733_pins_result a733_pins_acquire_banked_checked(struct a733_pins *m,uint64_t owner,
    const struct a733_pin_request_banked *r,size_t count,
    const struct a733_pin_output_condition *condition,
    struct a733_pin_lease_banked *out,uint64_t deadline)
{
    if (!condition) return A733_PINS_INVALID;
    return acquire_banked(m,owner,r,count,condition,out,deadline);
}
static enum a733_pins_result subset(struct a733_pins *m,const struct a733_pin_lease_banked *h,
    unsigned b,uint32_t mask,unsigned function,uint64_t deadline)
{
    if (!match_banked(m,h)) return A733_PINS_CONFLICT;
    if (m->failed) return A733_PINS_RECOVERY;
    if (!b || b>=A733_PIN_BANKS || !mask || (mask&~h->masks[b])) return A733_PINS_INVALID;
    for (unsigned p=0;p<32;++p) if (mask&(1u<<p)) {
        if (m->pins[b][p].current.function!=function) return A733_PINS_INVALID;
        enum a733_pins_result r=verify_pin(m,b,p,deadline);
        if (r!=A733_PINS_OK) return checked(m,r);
    }
    return A733_PINS_OK;
}
enum a733_pins_result a733_pins_release_banked(struct a733_pins *m,
    struct a733_pin_lease_banked *h,unsigned quiescent,uint64_t deadline)
{
    if (!m || !h || quiescent!=1 || overlap(m,sizeof(*m),h,sizeof(*h))) return A733_PINS_INVALID;
    enum a733_pins_result result=enter(m,deadline);
    if (result!=A733_PINS_OK) return result;
    if (!match_banked(m,h)) return leave(m,A733_PINS_CONFLICT);
    if (m->failed) return leave(m,A733_PINS_RECOVERY);
    struct expanded_plan plan[A733_PIN_MAX_REQUESTS]; size_t count=0;
    uint32_t deb_old[A733_PIN_BANKS]={0},deb_new[A733_PIN_BANKS]={0},deb_banks=0;
    uint32_t irqs[A733_PIN_BANKS]={0},enabled[A733_PIN_BANKS]={0};
    for (unsigned b=1;b<A733_PIN_BANKS;++b) for (unsigned p=0;p<32;++p) if (h->masks[b]&(1u<<p)) {
        if (count==A733_PIN_MAX_REQUESTS) return leave(m,A733_PINS_INVALID);
        result=verify_pin(m,b,p,deadline);
        if (result!=A733_PINS_OK) return leave(m,checked(m,result));
        struct a733_pin_state *s=&m->pins[b][p];
        plan[count++]=(struct expanded_plan){b,p,s->current,s->initial,s->initial_output,s->current_irq,s->initial_irq};
        if (s->current.function==14) {
            irqs[b]|=1u<<p;
            if (s->irq_enabled) enabled[b]|=1u<<p;
        }
    }
    for (unsigned b=1;b<A733_PIN_BANKS;++b) if (irqs[b]==m->banks[b].irq_pins && irqs[b]) {
        /* The bank-wide handoff excludes unmanaged interrupt clients. Verify
         * this again before restoring their common debounce configuration. */
        uint32_t v;
        if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
        if (!rd(m,irq_base(b)+0x10,&v) || (v&~irqs[b])) return leave(m,fault(m));
        deb_old[b]=m->banks[b].debounce; deb_new[b]=m->banks[b].initial_debounce; deb_banks|=1u<<b;
    }
    unsigned attempted=0;
    for (unsigned b=1;b<A733_PIN_BANKS;++b) if (irqs[b]) {
        result=rmw(m,irq_base(b)+0x10,irqs[b],enabled[b],0,deadline,&attempted);
        if (result!=A733_PINS_OK) return leave(m,result==A733_PINS_BUSY ? result : fault(m));
        /* Quiescent release owns these lines, including their pending latches. */
        result=put(m,irq_base(b)+0x14,irqs[b],deadline,&attempted);
        if (result!=A733_PINS_OK) return leave(m,fault(m));
    }
    result=apply_expanded(m,plan,count,deb_old,deb_new,deb_banks,deadline,&attempted);
    if (result!=A733_PINS_OK) return leave(m,result==A733_PINS_BUSY ? result : fault(m));
    for (size_t i=0;i<count;++i) m->pins[plan[i].b][plan[i].p]=(struct a733_pin_state){0};
    for (unsigned b=1;b<A733_PIN_BANKS;++b) m->banks[b].irq_pins&=~irqs[b];
    *h=(struct a733_pin_lease_banked){0};
    return leave(m,A733_PINS_OK);
}
static int observation_arguments(struct a733_pins *m,const struct a733_pin_lease_banked *h,uint32_t *out)
{
    return m && h && out && !overlap(m,sizeof(*m),out,sizeof(*out)) && !overlap(h,sizeof(*h),out,sizeof(*out));
}
enum a733_pins_result a733_pins_read_inputs_banked(struct a733_pins *m,
    const struct a733_pin_lease_banked *h,unsigned b,uint32_t mask,uint32_t *levels,uint64_t deadline)
{
    if (!observation_arguments(m,h,levels)) return A733_PINS_INVALID;
    enum a733_pins_result r=enter(m,deadline);
    if (r!=A733_PINS_OK) return r;
    r=subset(m,h,b,mask,0,deadline);
    if (r!=A733_PINS_OK) return leave(m,r);
    uint32_t value;
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    if (!rd(m,bank_base(b)+0x10,&value)) return leave(m,fault(m));
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    *levels=value&mask;
    return leave(m,A733_PINS_OK);
}
enum a733_pins_result a733_pins_write_outputs(struct a733_pins *m,
    const struct a733_pin_lease_banked *h,unsigned b,uint32_t mask,uint32_t levels,uint64_t deadline)
{
    if (!m || !h || (levels&~mask)) return A733_PINS_INVALID;
    enum a733_pins_result r=enter(m,deadline);
    if (r!=A733_PINS_OK) return r;
    r=subset(m,h,b,mask,1,deadline);
    if (r!=A733_PINS_OK) return leave(m,r);
    unsigned attempted=0;
    r=output_latch(m,b,mask,levels,deadline,&attempted);
    if (r!=A733_PINS_OK) return leave(m,r==A733_PINS_BUSY ? r : fault(m));
    uint32_t v;
    if (!rd(m,bank_base(b)+0x10,&v) || (v&mask)!=levels || !in_time(m,deadline)) return leave(m,fault(m));
    for (unsigned p=0;p<32;++p) if (mask&(1u<<p)) m->pins[b][p].current_output=(levels>>p)&1;
    return leave(m,A733_PINS_OK);
}
enum a733_pins_result a733_pins_irq_enable(struct a733_pins *m,
    const struct a733_pin_lease_banked *h,unsigned b,uint32_t mask,unsigned enable,uint64_t deadline)
{
    if (!m || !h || enable>1) return A733_PINS_INVALID;
    enum a733_pins_result r=enter(m,deadline);
    if (r!=A733_PINS_OK) return r;
    r=subset(m,h,b,mask,14,deadline);
    if (r!=A733_PINS_OK) return leave(m,r);
    uint32_t expected=0;
    for (unsigned p=0;p<32;++p) if ((mask&(1u<<p)) && m->pins[b][p].irq_enabled) expected|=1u<<p;
    unsigned attempted=0;
    r=rmw(m,irq_base(b)+0x10,mask,expected,enable ? mask : 0,deadline,&attempted);
    if (r!=A733_PINS_OK) return leave(m,r==A733_PINS_BUSY ? r : fault(m));
    for (unsigned p=0;p<32;++p) if (mask&(1u<<p)) m->pins[b][p].irq_enabled=enable;
    return leave(m,A733_PINS_OK);
}
enum a733_pins_result a733_pins_irq_pending(struct a733_pins *m,
    const struct a733_pin_lease_banked *h,unsigned b,uint32_t mask,uint32_t *pending,uint64_t deadline)
{
    if (!observation_arguments(m,h,pending)) return A733_PINS_INVALID;
    enum a733_pins_result r=enter(m,deadline);
    if (r!=A733_PINS_OK) return r;
    r=subset(m,h,b,mask,14,deadline);
    if (r!=A733_PINS_OK) return leave(m,r);
    uint32_t v;
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    if (!rd(m,irq_base(b)+0x14,&v)) return leave(m,fault(m));
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    *pending=v&mask;
    return leave(m,A733_PINS_OK);
}
enum a733_pins_result a733_pins_irq_ack(struct a733_pins *m,
    const struct a733_pin_lease_banked *h,unsigned b,uint32_t mask,uint64_t deadline)
{
    if (!m || !h) return A733_PINS_INVALID;
    enum a733_pins_result r=enter(m,deadline);
    if (r!=A733_PINS_OK) return r;
    r=subset(m,h,b,mask,14,deadline);
    if (r!=A733_PINS_OK) return leave(m,r);
    unsigned attempted=0;
    r=put(m,irq_base(b)+0x14,mask,deadline,&attempted);
    /* A level or new edge may immediately reassert; zero readback is invalid. */
    return leave(m,r==A733_PINS_OK || r==A733_PINS_BUSY ? r : fault(m));
}
enum a733_pins_result a733_pins_held_banked(struct a733_pins *m,
    const struct a733_pin_lease_banked *h,int *held,uint64_t deadline)
{
    if (!m || !h || !held || overlap(m,sizeof(*m),held,sizeof(*held)) ||
        overlap(h,sizeof(*h),held,sizeof(*held))) return A733_PINS_INVALID;
    enum a733_pins_result r=enter(m,deadline);
    if (r!=A733_PINS_OK) return r;
    *held=match_banked(m,h);
    return leave(m,A733_PINS_OK);
}

enum a733_pins_result a733_pins_pwm0_fan_low(struct a733_pins *m,
    const struct a733_pin_lease_banked *h,unsigned quiescent,uint64_t deadline)
{
    if(!m||!h||quiescent!=1||overlap(m,sizeof(*m),h,sizeof(*h)))return A733_PINS_INVALID;
    enum a733_pins_result r=enter(m,deadline);
    if(r!=A733_PINS_OK)return r;
    if(!match_banked(m,h))return leave(m,A733_PINS_CONFLICT);
    if(m->failed)return leave(m,A733_PINS_RECOVERY);
    if(!(h->masks[A733_PIN_D]&(1u<<5)))return leave(m,A733_PINS_INVALID);
    struct a733_pin_state *s=&m->pins[A733_PIN_D][5];
    if(s->current.function!=7 && !(s->current.function==1&&!s->current_output))return leave(m,A733_PINS_INVALID);
    r=verify_pin(m,A733_PIN_D,5,deadline);
    if(r!=A733_PINS_OK)return leave(m,checked(m,r));
    if(s->current.function==1)return leave(m,A733_PINS_OK);
    struct expanded_plan plan={A733_PIN_D,5,s->current,{1,s->current.pull,s->current.drive},0,0,0};
    uint32_t empty[A733_PIN_BANKS]={0};unsigned attempted=0;
    r=apply_expanded(m,&plan,1,empty,empty,0,deadline,&attempted);
    if(r!=A733_PINS_OK)return leave(m,r==A733_PINS_BUSY&&!attempted?r:fault(m));
    s->current=plan.target;s->current_output=0;
    return leave(m,A733_PINS_OK);
}
