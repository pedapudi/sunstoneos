#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct fixture {
    struct a733_clock_leases registry;
    uint64_t now;
    unsigned fail,contend,calls;
};
static int now(void *v,uint64_t *out)
{
    struct fixture *f=v;++f->calls;
    if(f->contend) {
        struct a733_clock_token untouched={.id=999};
        assert(a733_clock_acquire(&f->registry,99,0,A733_CLOCK_PLL_NPU,100,&untouched)==A733_CLOCK_BUSY);
        assert(untouched.id==999);
    }
    if(f->fail)return 0;
    *out=f->now;return 1;
}
static int same(const struct a733_clock_token *a,const struct a733_clock_token *b)
{return a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive;}
static void setup(struct fixture *f,struct a733_clock_token *t,unsigned display)
{
    *f=(struct fixture){.now=1};assert(a733_clock_leases_init(&f->registry,now,f));
    a733_clock_mask_t exclusive=display ? A733_CLOCK_DISPLL1|A733_CLOCK_VO0 : A733_CLOCK_PLL_NPU|A733_CLOCK_NPU;
    assert(a733_clock_acquire(&f->registry,7,A733_CLOCK_FABRIC|A733_CLOCK_PERI0,exclusive,100,t)==A733_CLOCK_OK);
}
int main(void)
{
    struct fixture f;struct a733_clock_token old,next;
    for(unsigned display=0;display<2;++display) {
        setup(&f,&old,display);f.contend=1;
        a733_clock_mask_t parent=display ? A733_CLOCK_DISPLL1 : A733_CLOCK_PLL_NPU;
        assert(a733_clock_publish_stable(&f.registry,&old,parent,100,&next)==A733_CLOCK_OK);
        assert(next.id!=old.id && next.owner==old.owner && next.shared==(old.shared|parent) &&
            next.exclusive==(old.exclusive&~parent));
        assert(a733_clock_token_status(&f.registry,&old)==A733_CLOCK_INVALID);
        assert(a733_clock_token_status(&f.registry,&next)==A733_CLOCK_OK);
        assert(a733_clock_release(&f.registry,&old,100)==A733_CLOCK_INVALID);
        struct a733_clock_token consumer;
        assert(a733_clock_acquire(&f.registry,8,parent,0,100,&consumer)==A733_CLOCK_OK);
        assert(a733_clock_acquire(&f.registry,8,0,parent,100,&consumer)==A733_CLOCK_BUSY);
        assert(a733_clock_acquire(&f.registry,8,0,next.exclusive,100,&consumer)==A733_CLOCK_BUSY);
        struct a733_clock_token output={.id=999};
        assert(a733_clock_publish_stable(&f.registry,&old,parent,100,&output)==A733_CLOCK_INVALID && output.id==999);
    }
    for(unsigned reason=0;reason<8;++reason) {
        setup(&f,&old,0);struct a733_clock_token output={.id=999},saved=old;
        a733_clock_mask_t parent=A733_CLOCK_PLL_NPU;uint64_t deadline=100;
        enum a733_clock_result expected=A733_CLOCK_INVALID;
        switch(reason) {
        case 0:parent=0;break;
        case 1:parent=A733_CLOCK_REF;break;
        case 2:++old.owner;break;
        case 3:deadline=1;expected=A733_CLOCK_EXPIRED;break;
        case 4:f.fail=1;expected=A733_CLOCK_FAULT;break;
        case 5:atomic_store(&f.registry.busy,1);expected=A733_CLOCK_BUSY;break;
        case 6:f.registry.next_id=0;expected=A733_CLOCK_EXHAUSTED;break;
        case 7:parent=A733_CLOCK_DISPLL1;break;
        }
        assert(a733_clock_publish_stable(&f.registry,&old,parent,deadline,&output)==expected);
        assert(output.id==999 && same(&f.registry.slots[0],&saved));
    }
    setup(&f,&old,0);
    assert(a733_clock_publish_stable(&f.registry,&old,A733_CLOCK_PLL_NPU,100,&old)==A733_CLOCK_INVALID);
    assert(a733_clock_publish_stable(&f.registry,&old,A733_CLOCK_PLL_NPU,100,&f.registry.slots[1])==A733_CLOCK_INVALID);
    assert(a733_clock_publish_stable(&f.registry,&f.registry.slots[0],A733_CLOCK_PLL_NPU,100,&next)==A733_CLOCK_INVALID);
    assert(a733_clock_token_status(&f.registry,&old)==A733_CLOCK_OK);
    setup(&f,&old,0);
    struct a733_clock_token thermal;
    assert(a733_clock_acquire(&f.registry,8,A733_CLOCK_THS,0,100,&thermal)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&f.registry,8,0,A733_CLOCK_THS,100,&thermal)==A733_CLOCK_OK);
    assert(thermal.shared==(A733_CLOCK_GPADC24M|A733_CLOCK_REF|A733_CLOCK_OSC));
    struct a733_clock_token blocked;
    assert(a733_clock_acquire(&f.registry,9,0,A733_CLOCK_GPADC24M,100,&blocked)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&f.registry,9,0,A733_CLOCK_REF,100,&blocked)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&f.registry,9,0,A733_CLOCK_OSC,100,&blocked)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&f.registry,9,A733_CLOCK_GPADC24M,0,100,&blocked)==A733_CLOCK_OK);
    puts("Thermal local exclusion and shared ADC/reference/oscillator ancestry PASS");
    puts("NPU/display atomic publication, competing admissions, stale tokens, all refusals and alias guards PASS");
    return 0;
}
