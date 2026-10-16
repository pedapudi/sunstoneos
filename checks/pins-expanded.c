#include "a733_pins_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct a733_pins m;
static uint32_t main_words[0x600/4],r_words[0x410/4];
static uint32_t latches[A733_PIN_BANKS],inputs[A733_PIN_BANKS],primed[A733_PIN_BANKS];
static unsigned locked,reads,writes,fail_write,fail_read,expire_write,reenter_write;
static unsigned ignore_alias,late_read,reassert_irq;
static uint64_t time_now;
static uintptr_t last_write,expire_address;
static uint32_t last_value;
static uintptr_t base(unsigned b)
{ return b<A733_PIN_L ? A733_PINS_BASE+0x80+b*0x80 : A733_PINS_R_BASE+(b-A733_PIN_L)*0x30; }
static uintptr_t irq(unsigned b)
{ return b<A733_PIN_L ? base(b)+0x40 : A733_PINS_R_BASE+0x200+(b-A733_PIN_L)*0x20; }
static uint32_t *word(uintptr_t a)
{
    if (a>=A733_PINS_R_BASE) { assert(a<A733_PINS_R_BASE+sizeof(r_words)); return &r_words[(a-A733_PINS_R_BASE)/4]; }
    assert(a>=A733_PINS_BASE && a<A733_PINS_BASE+sizeof(main_words));
    return &main_words[(a-A733_PINS_BASE)/4];
}
static unsigned mux(unsigned b,unsigned p)
{ return (*word(base(b)+(p/8)*4)>>((p%8)*4))&15; }
static void config(unsigned b,unsigned p,unsigned fn,unsigned pull,unsigned drive)
{
    struct a733_pin_fields f; assert(a733_pin_fields_banked(b,p,&f));
    *word(f.mux_address)=(*word(f.mux_address)&~(15u<<f.mux_shift))|(fn<<f.mux_shift);
    *word(f.drive_address)=(*word(f.drive_address)&~(15u<<f.drive_shift))|(drive<<f.drive_shift);
    *word(f.pull_address)=(*word(f.pull_address)&~(3u<<f.pull_shift))|(pull<<f.pull_shift);
}
static int enter_model(void *cookie,uint64_t deadline)
{ assert(cookie==&m); if (locked || time_now>=deadline) return 0; locked=1; return 1; }
static void leave_model(void *cookie)
{ assert(cookie==&m && locked); locked=0; }
static int now_model(void *cookie,uint64_t *out)
{ assert(cookie==&m); *out=time_now; return 1; }
static int read_model(void *cookie,uintptr_t a,uint32_t *out)
{
    assert(cookie==&m && locked && a733_pins_native_address(a,0));
    if (++reads==fail_read) return 0;
    for (unsigned b=1;b<A733_PIN_BANKS;++b) if (a==base(b)+0x10) {
        uint32_t value=0;
        for (unsigned p=0;p<32;++p) if (a733_pin_bank_mask(b)&(1u<<p)) {
            unsigned fn=mux(b,p);
            if (fn==1) value|=latches[b]&(1u<<p);
            else if (fn==0) value|=inputs[b]&(1u<<p);
            else value|=UINT32_C(0xa5a5a5a5)&(1u<<p); /* Undefined alternate DATA. */
        }
        *out=value;
        if (late_read) time_now=100;
        return 1;
    }
    *out=*word(a); return 1;
}
static uint32_t owned(unsigned b)
{
    uint32_t mask=0;
    for (unsigned p=0;p<32;++p) if (m.pins[b][p].id) mask|=1u<<p;
    return mask;
}
static int write_model(void *cookie,uintptr_t a,uint32_t v)
{
    assert(cookie==&m && locked && a733_pins_native_address(a,1));
    ++writes; last_write=a; last_value=v;
    if (reenter_write) {
        struct a733_pin_lease_banked none={0}; int held=4;
        assert(a733_pins_held_banked(&m,&none,&held,100)==A733_PINS_BUSY && held==4);
    }
    unsigned handled=0;
    for (unsigned b=1;b<A733_PIN_BANKS;++b) {
        uint32_t mask=owned(b);
        if ((b<A733_PIN_L && (a==base(b)+0x14 || a==base(b)+0x18)) ||
            (b>=A733_PIN_L && a==base(b)+0x10)) {
            assert(mask);
            if (b>=A733_PIN_L) {
                assert(!((latches[b]^v)&~mask));
                if (!ignore_alias) latches[b]=v;
            } else {
                assert(!(v&~mask));
                if (!ignore_alias) {
                    if (a==base(b)+0x14) latches[b]|=v;
                    else latches[b]&=~v;
                }
            }
            primed[b]|=b>=A733_PIN_L ? mask : v;
            handled=1; break;
        }
        if (a==irq(b)+0x14) {
            assert(mask && !(v&~mask));
            *word(a)&=~v;
            if (reassert_irq) *word(a)|=v;
            handled=1; break;
        }
        if (a==irq(b)+0x10) {
            assert(!((*word(a)^v)&~mask)); *word(a)=v; handled=1; break;
        }
        if (a==irq(b)+0x18) {
            assert(m.handoff.irq_banks&(1u<<b));
            assert(!((*word(a)^v)&~UINT32_C(0x71))); *word(a)=v; handled=1; break;
        }
        uint32_t allowed=0;
        for (unsigned p=0;p<32;++p) if (mask&(1u<<p)) {
            struct a733_pin_fields f; assert(a733_pin_fields_banked(b,p,&f));
            if (a==f.mux_address) {
                allowed|=15u<<f.mux_shift;
                if (((v>>f.mux_shift)&15)==1) {
                    /* Selection must follow latch preload, including restore. */
                    assert(primed[b]&(1u<<p));
                    unsigned bit=(latches[b]>>p)&1;
                    struct a733_pin_state *s=&m.pins[b][p];
                    assert(bit==s->current_output || bit==s->initial_output);
                }
            }
            if (a==f.drive_address) allowed|=15u<<f.drive_shift;
            if (a==f.pull_address) allowed|=3u<<f.pull_shift;
            if (a==irq(b)+(p/8)*4) allowed|=15u<<((p%8)*4);
        }
        if (allowed) { assert(!((*word(a)^v)&~allowed)); *word(a)=v; handled=1; break; }
    }
    assert(handled);
    if (writes==expire_write || a==expire_address) time_now=100;
    return writes!=fail_write;
}
static void setup(unsigned latch_authority)
{
    assert(!locked); memset(&m,0,sizeof(m)); memset(main_words,0,sizeof(main_words)); memset(r_words,0,sizeof(r_words));
    memset(latches,0,sizeof(latches)); memset(inputs,0,sizeof(inputs)); memset(primed,0,sizeof(primed));
    reads=writes=fail_write=fail_read=expire_write=reenter_write=ignore_alias=late_read=reassert_irq=0;
    expire_address=last_write=0; time_now=0;
    struct a733_pin_handoff h={0};
    for (unsigned b=1;b<A733_PIN_BANKS;++b) {
        h.authorized[b]=a733_pin_bank_mask(b); h.irq_banks|=1u<<b;
        for (unsigned p=0;p<32;++p) if (h.authorized[b]&(1u<<p)) config(b,p,15,1,2);
        *word(irq(b)+0x18)=0x80000000;
    }
    if (latch_authority) for (unsigned b=A733_PIN_L;b<=A733_PIN_M;++b) {
        h.latch[b-A733_PIN_L]=(struct a733_pin_latch_handoff){10+b,88,a733_pin_bank_mask(b),1u<<1};
        latches[b]=1u<<1;
        config(b,1,1,0,1); /* Unleased neighbor with an authenticated high latch. */
    }
    struct a733_pins_operations ops={enter_model,leave_model,now_model,read_model,write_model};
    assert(a733_pins_init_banks(&m,&ops,&m,&h));
}
static struct a733_pin_request_banked req(unsigned b,unsigned p,unsigned fn)
{ return (struct a733_pin_request_banked){b,p,{15,1,2},{fn,0,1},0,0,0}; }
static void retained(struct a733_pin_lease_banked *h)
{
    int held=0; assert(h->id && m.failed);
    assert(a733_pins_held_banked(&m,h,&held,1000)==A733_PINS_OK && held);
    unsigned n=reads,w=writes;
    assert(a733_pins_release_banked(&m,h,1,1000)==A733_PINS_RECOVERY);
    assert(n==reads && w==writes);
}
int main(void)
{
    setup(0);
    struct a733_pin_request_banked bt[4];
    for (unsigned i=0;i<4;++i) bt[i]=req(A733_PIN_G,6+i,2);
    struct a733_pin_lease_banked a={0},b={0};
    assert(a733_pins_acquire_banked(&m,1,bt,4,&a,100)==A733_PINS_OK);
    struct a733_pin_request_banked header=req(A733_PIN_E,12,8);
    unsigned before=writes;
    assert(a733_pins_acquire_banked(&m,2,&header,1,&b,100)==A733_PINS_CONFLICT && writes==before && !b.id);
    struct a733_pin_lease_banked partial=a; partial.masks[A733_PIN_G]&=~(1u<<6);
    assert(a733_pins_release_banked(&m,&partial,1,100)==A733_PINS_CONFLICT);
    assert(a733_pins_release_banked(&m,&a,1,100)==A733_PINS_OK);
    struct a733_pin_request_banked mixed[2]={bt[0],header};
    assert(a733_pins_acquire_banked(&m,1,mixed,2,&a,100)==A733_PINS_INVALID);
    /* A controller's inherited route remains reserved until restoration. */
    config(A733_PIN_E,12,8,1,2); header.expected.function=8; header.desired.function=0;
    assert(a733_pins_acquire_banked(&m,1,&header,1,&a,100)==A733_PINS_OK);
    assert(a733_pins_acquire_banked(&m,2,bt,4,&b,100)==A733_PINS_CONFLICT);
    assert(a733_pins_release_banked(&m,&a,1,100)==A733_PINS_OK);
    setup(0); struct a733_pin_request_banked spi=req(A733_PIN_E,0,5),uart=req(A733_PIN_E,0,8);
    assert(a733_pins_acquire_banked(&m,1,&spi,1,&a,100)==A733_PINS_OK);
    assert(a733_pins_acquire_banked(&m,2,&uart,1,&b,100)==A733_PINS_CONFLICT);
    assert(a733_pins_release_banked(&m,&a,1,100)==A733_PINS_OK);

    /* Route identity follows signal maps, including nonadjacent pairs. */
    struct a733_pin_request_banked pair[2]={req(A733_PIN_F,2,3),req(A733_PIN_F,4,3)};
    assert(a733_pins_acquire_banked(&m,1,pair,2,&a,100)==A733_PINS_OK);
    assert(a733_pins_release_banked(&m,&a,1,100)==A733_PINS_OK);
    pair[0]=req(A733_PIN_M,3,5); pair[1]=req(A733_PIN_M,5,5);
    assert(a733_pins_acquire_banked(&m,1,pair,2,&a,100)==A733_PINS_INVALID);
    pair[0]=req(A733_PIN_I,0,3);
    assert(a733_pins_acquire_banked(&m,1,pair,1,&a,100)==A733_PINS_INVALID);

    /* Both camera connector groups, including control and clock pins. */
    setup(0); struct a733_pin_request_banked camera[28]; size_t n=0;
    for (unsigned p=0;p<20;++p) camera[n++]=req(A733_PIN_K,p,2);
    camera[n++]=req(A733_PIN_E,5,2); camera[n++]=req(A733_PIN_E,6,1);
    camera[n++]=req(A733_PIN_E,7,8); camera[n++]=req(A733_PIN_E,8,8);
    camera[n++]=req(A733_PIN_E,9,2); camera[n++]=req(A733_PIN_E,10,1);
    camera[n++]=req(A733_PIN_E,14,8); camera[n++]=req(A733_PIN_E,15,8);
    assert(n==28); reenter_write=1;
    assert(a733_pins_acquire_banked(&m,1,camera,n,&a,100)==A733_PINS_OK);
    assert(a733_pins_write_outputs(&m,&a,A733_PIN_E,(1u<<6)|(1u<<10),1u<<6,100)==A733_PINS_OK);
    assert(latches[A733_PIN_E]&(1u<<6));
    assert(a733_pins_release_banked(&m,&a,1,100)==A733_PINS_OK);
    /* PB APIs contend through the same state but cannot convert token formats. */
    struct a733_pin_request old={5,{15,1,2},{3,0,1}}; struct a733_pin_lease legacy={0};
    assert(a733_pins_acquire(&m,3,&old,1,&legacy,100)==A733_PINS_OK);
    struct a733_pin_request_banked pb=req(A733_PIN_B,5,0);
    assert(a733_pins_acquire_banked(&m,4,&pb,1,&a,100)==A733_PINS_CONFLICT);
    assert(a733_pins_release(&m,&legacy,1,100)==A733_PINS_OK);
    assert(a733_pins_acquire_banked(&m,4,&pb,1,&a,100)==A733_PINS_OK);
    legacy=(struct a733_pin_lease){a.id,a.owner,a.masks[A733_PIN_B]};
    assert(a733_pins_release(&m,&legacy,1,100)==A733_PINS_CONFLICT);
    assert(a733_pins_release_banked(&m,&a,1,100)==A733_PINS_OK);

    setup(0); struct a733_pin_request_banked out=req(A733_PIN_D,1,1); out.output_value=0;
    out.expected.function=1; config(A733_PIN_D,1,1,1,2); latches[A733_PIN_D]=(1u<<1)|(1u<<20);
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_OK);
    assert(latches[A733_PIN_D]==(1u<<20));
    assert(a733_pins_release_banked(&m,&a,1,100)==A733_PINS_OK);
    assert(latches[A733_PIN_D]==((1u<<1)|(1u<<20)));
    setup(0); out=req(A733_PIN_L,2,1); before=writes;
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_INVALID && !a.id && writes==before);
    setup(1); out.output_value=1;
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_OK);
    assert(latches[A733_PIN_L]==((1u<<1)|(1u<<2)));
    struct a733_pin_request_banked in=req(A733_PIN_L,3,0);
    assert(a733_pins_acquire_banked(&m,2,&in,1,&b,100)==A733_PINS_OK);
    inputs[A733_PIN_L]=1u<<3; uint32_t value=99;
    assert(a733_pins_read_inputs_banked(&m,&b,A733_PIN_L,1u<<3,&value,100)==A733_PINS_OK && value==(1u<<3));
    assert(a733_pins_write_outputs(&m,&a,A733_PIN_L,1u<<2,0,100)==A733_PINS_OK);
    assert(latches[A733_PIN_L]==(1u<<1)); /* Input sample did not enter shadow. */
    late_read=1; value=99;
    assert(a733_pins_read_inputs_banked(&m,&b,A733_PIN_L,1u<<3,&value,100)==A733_PINS_BUSY && value==99 && !m.failed);
    late_read=0; time_now=0; /* Fresh fixture clock for the following case. */
    setup(1); a=(struct a733_pin_lease_banked){0}; b=(struct a733_pin_lease_banked){0};
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_OK);
    latches[A733_PIN_L]&=~(1u<<1); before=writes;
    assert(a733_pins_write_outputs(&m,&a,A733_PIN_L,1u<<2,0,100)==A733_PINS_RECOVERY && writes==before);
    retained(&a);

    setup(0); a=(struct a733_pin_lease_banked){0};
    struct a733_pin_request_banked e=req(A733_PIN_K,23,14); e.irq_mode=4; e.debounce=0x31;
    assert(a733_pins_acquire_banked(&m,1,&e,1,&a,100)==A733_PINS_OK);
    assert(!*word(irq(A733_PIN_K)+0x10));
    struct a733_pin_request_banked e2=req(A733_PIN_K,22,14); e2.irq_mode=1; e2.debounce=0x21;
    assert(a733_pins_acquire_banked(&m,2,&e2,1,&b,100)==A733_PINS_CONFLICT);
    e2.debounce=e.debounce;
    assert(a733_pins_acquire_banked(&m,2,&e2,1,&b,100)==A733_PINS_OK);
    assert(a733_pins_irq_enable(&m,&a,A733_PIN_K,1u<<23,1,100)==A733_PINS_OK);
    assert(a733_pins_irq_enable(&m,&b,A733_PIN_K,1u<<22,1,100)==A733_PINS_OK);
    *word(irq(A733_PIN_K)+0x14)=(1u<<23)|(1u<<22)|(1u<<21); value=0;
    assert(a733_pins_irq_pending(&m,&a,A733_PIN_K,1u<<23,&value,100)==A733_PINS_OK && value==(1u<<23));
    assert(a733_pins_irq_ack(&m,&a,A733_PIN_K,1u<<22,100)==A733_PINS_INVALID);
    assert(a733_pins_irq_ack(&m,&a,A733_PIN_K,1u<<23,100)==A733_PINS_OK);
    assert(*word(irq(A733_PIN_K)+0x14)==((1u<<22)|(1u<<21)));
    reassert_irq=1;
    assert(a733_pins_irq_ack(&m,&a,A733_PIN_K,1u<<23,100)==A733_PINS_OK);
    assert(*word(irq(A733_PIN_K)+0x14)&(1u<<23)); reassert_irq=0;
    assert(a733_pins_release_banked(&m,&a,1,100)==A733_PINS_OK);
    assert(*word(irq(A733_PIN_K)+0x10)==(1u<<22));
    assert((*word(irq(A733_PIN_K)+0x18)&0x71)==0x31);
    assert(a733_pins_release_banked(&m,&b,1,100)==A733_PINS_OK);
    assert(*word(irq(A733_PIN_K)+0x18)==0x80000000);
    assert(*word(irq(A733_PIN_K)+0x14)==(1u<<21));
    /* Always-on IRQ layout and edge mode share the same native authority. */
    e=req(A733_PIN_M,4,14); e.irq_mode=0;
    assert(a733_pins_acquire_banked(&m,1,&e,1,&a,100)==A733_PINS_OK);
    assert(a733_pins_irq_enable(&m,&a,A733_PIN_M,1u<<4,1,100)==A733_PINS_OK);
    expire_address=irq(A733_PIN_M)+0x14;
    assert(a733_pins_irq_ack(&m,&a,A733_PIN_M,1u<<4,100)==A733_PINS_RECOVERY);
    retained(&a);

    /* Final output readback, uncertain writes, final mux deadlines and faults. */
    setup(0); a=(struct a733_pin_lease_banked){0}; out=req(A733_PIN_E,6,1); out.output_value=1;
    ignore_alias=1;
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_RECOVERY); retained(&a);
    setup(0); a=(struct a733_pin_lease_banked){0}; fail_write=1;
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_RECOVERY); retained(&a);
    setup(0); a=(struct a733_pin_lease_banked){0}; expire_address=base(A733_PIN_E);
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_RECOVERY); retained(&a);
    setup(0); a=(struct a733_pin_lease_banked){0}; fail_read=1;
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_RECOVERY && !a.id && !writes);
    setup(0); a=(struct a733_pin_lease_banked){0}; time_now=100;
    assert(a733_pins_acquire_banked(&m,1,&out,1,&a,100)==A733_PINS_BUSY && !a.id && !reads && !writes);
    assert(!a733_pins_native_address(A733_PINS_BASE+0x48,1));
    assert(!a733_pins_native_address(A733_PINS_R_BASE+0x340,1));
    assert(!a733_pins_native_address(A733_PINS_BASE+0x110,1));
    puts("Expanded pins: camera/UART leases, GPIO input/output, latch authority, owned IRQ/debounce, legacy contention, reentry and retained faults passed");
    return 0;
}
