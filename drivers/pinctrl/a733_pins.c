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
    if (!m || m->initialized || !ops || !ops->enter || !ops->leave || !ops->now ||
        !ops->read || !ops->write || !authorized || (authorized & ~A733_PINS_PB_MASK)) return 0;
    m->operations = *ops; m->cookie = cookie;
    m->authorized_pb_mask = authorized; m->next_id = 1; m->initialized = 1;
    return 1;
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
    for (unsigned i=0; i<A733_PINS_COUNT; ++i)
        if (m->pins[i].id == h->id && m->pins[i].owner == h->owner) found |= 1u << (i+4);
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
        if ((mask & (1u<<(i+4))) && m->pins[i].id) return leave(m,A733_PINS_CONFLICT);
    result=preflight(m,&p,mask,deadline);
    if (result!=A733_PINS_OK) return leave(m,result);
    if (!in_time(m,deadline)) return leave(m,A733_PINS_BUSY);
    *out=(struct a733_pin_lease){m->next_id++,owner,mask};
    for (size_t i=0; i<count; ++i)
        m->pins[r[i].pb_pin-4]=(struct a733_pin_state){out->id,owner,r[i].expected,r[i].desired};
    unsigned attempted=0;
    result=apply(m,&p,deadline,&attempted);
    if (result==A733_PINS_BUSY) {
        for (unsigned i=0; i<A733_PINS_COUNT; ++i)
            if (mask & (1u<<(i+4))) m->pins[i]=(struct a733_pin_state){0};
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
        add_fields(&p,i+4,m->pins[i].current,m->pins[i].initial);
    result=preflight(m,&p,h->pb_mask,deadline);
    if (result==A733_PINS_BUSY) return leave(m,result);
    if (result!=A733_PINS_OK) return leave(m,fault(m));
    unsigned attempted=0;
    result=apply(m,&p,deadline,&attempted);
    if (result==A733_PINS_BUSY) return leave(m,result);
    if (result!=A733_PINS_OK) return leave(m,fault(m));
    for (unsigned i=0; i<A733_PINS_COUNT; ++i)
        if (h->pb_mask & (1u<<(i+4))) m->pins[i]=(struct a733_pin_state){0};
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
        if (m->pins[i].current.function!=0) return leave(m,A733_PINS_INVALID);
        add_fields(&p,i+4,m->pins[i].current,m->pins[i].current);
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
