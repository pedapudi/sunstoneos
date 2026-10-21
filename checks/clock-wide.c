#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned observations;
static int now(void *cookie,uint64_t *out)
{(void)cookie;++observations;*out=1;return 1;}
static void same(const struct a733_clock_token *a,const struct a733_clock_token *b)
{assert(a->id==b->id && a->owner==b->owner && a->shared==b->shared && a->exclusive==b->exclusive);}

int main(void)
{
    _Static_assert(A733_CLOCK_VO1==((a733_clock_mask_t)1<<63),"existing highest scope");
    _Static_assert(A733_CLOCK_TWI11==((a733_clock_mask_t)1<<64),"independent TWI11 scope");
    _Static_assert(A733_CLOCK_TWI9==((a733_clock_mask_t)1<<77),"independent TWI9 scope");
    _Static_assert(A733_CLOCK_AUDIO_CHANGE==0x3ff8,"existing audio values");
    _Static_assert(sizeof(a733_clock_mask_t)==16 && sizeof(struct a733_clock_token)==48,"wide private ABI");
    struct a733_clock_leases registry={0};
    struct a733_clock_token twi,twi9,vo1,out={.id=999},saved=out;
    assert(a733_clock_leases_init(&registry,now,NULL));
    assert(a733_clock_acquire(&registry,1,0,A733_CLOCK_TWI11,10,&twi)==A733_CLOCK_OK);
    assert(twi.exclusive==A733_CLOCK_TWI11 && twi.shared==A733_CLOCK_FABRIC);
    assert(a733_clock_acquire(&registry,2,0,A733_CLOCK_VO1,10,&vo1)==A733_CLOCK_OK);
    assert(vo1.exclusive==A733_CLOCK_VO1 && !vo1.shared);
    /* TWI9 is a second camera bus: independent of TWI11, sharing FABRIC. */
    assert(a733_clock_acquire(&registry,6,0,A733_CLOCK_TWI9,10,&twi9)==A733_CLOCK_OK);
    assert(twi9.exclusive==A733_CLOCK_TWI9 && twi9.shared==A733_CLOCK_FABRIC);
    unsigned before=observations;
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_TWI9,10,&out)==A733_CLOCK_BUSY);
    same(&out,&saved);
    assert(a733_clock_acquire(&registry,3,A733_CLOCK_TWI9,0,10,&out)==A733_CLOCK_INVALID);
    same(&out,&saved);
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_TWI11,10,&out)==A733_CLOCK_BUSY);
    same(&out,&saved);
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_FABRIC,10,&out)==A733_CLOCK_BUSY);
    same(&out,&saved);
    assert(a733_clock_acquire(&registry,3,A733_CLOCK_TWI11,0,10,&out)==A733_CLOCK_INVALID);
    same(&out,&saved);
    assert(observations==before);
    for(unsigned bit=0;bit<128;++bit) {
        if(A733_CLOCK_BIT(bit)&A733_CLOCK_ALL)continue;
        assert(a733_clock_acquire(&registry,3,A733_CLOCK_BIT(bit),0,10,&out)==A733_CLOCK_INVALID);
        assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_BIT(bit),10,&out)==A733_CLOCK_INVALID);
        same(&out,&saved);
    }
    assert(observations==before);
    /* Only upper-half mutation differs from the authentic low-bit token. */
    struct a733_clock_token forged=vo1;
    forged.exclusive|=A733_CLOCK_TWI11;
    assert(a733_clock_token_status(&registry,&forged)==A733_CLOCK_INVALID);
    assert(a733_clock_release(&registry,&forged,10)==A733_CLOCK_INVALID);
    assert(a733_clock_retain(&registry,&forged)==A733_CLOCK_INVALID);
    forged=vo1;forged.shared|=A733_CLOCK_TWI11;
    assert(a733_clock_token_status(&registry,&forged)==A733_CLOCK_INVALID);
    assert(a733_clock_release(&registry,&forged,10)==A733_CLOCK_INVALID);
    assert(a733_clock_retain(&registry,&forged)==A733_CLOCK_INVALID);
    assert(a733_clock_token_status(&registry,&twi)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&registry,&vo1)==A733_CLOCK_OK);
    /* A busy metadata lock refuses immediately without consulting time. */
    atomic_store_explicit(&registry.busy,1,memory_order_relaxed);
    assert(a733_clock_release(&registry,&twi,10)==A733_CLOCK_BUSY);
    assert(observations==before);
    atomic_store_explicit(&registry.busy,0,memory_order_release);
    assert(a733_clock_release(&registry,&twi,1)==A733_CLOCK_EXPIRED);
    assert(a733_clock_token_status(&registry,&twi)==A733_CLOCK_OK);
    assert(a733_clock_release(&registry,&twi,10)==A733_CLOCK_OK);
    assert(a733_clock_release(&registry,&vo1,10)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&registry,&twi9)==A733_CLOCK_OK);
    assert(a733_clock_release(&registry,&twi9,10)==A733_CLOCK_OK);

    /* Actual stable publication must preserve both halves of unrelated scopes. */
    const a733_clock_mask_t locals=A733_CLOCK_TWI11|A733_CLOCK_VO1;
    struct a733_clock_token old,next={.id=999};
    assert(a733_clock_acquire(&registry,4,0,locals|A733_CLOCK_PLL_NPU,10,&old)==A733_CLOCK_OK);
    assert(old.shared==(A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_publish_stable(&registry,&old,A733_CLOCK_PLL_NPU,1,&next)==A733_CLOCK_EXPIRED);
    same(&next,&saved);
    assert(a733_clock_token_status(&registry,&old)==A733_CLOCK_OK);
    assert(a733_clock_publish_stable(&registry,&old,A733_CLOCK_PLL_NPU,10,&next)==A733_CLOCK_OK);
    assert(next.id!=old.id && next.owner==old.owner && next.exclusive==locals);
    assert(next.shared==(old.shared|A733_CLOCK_PLL_NPU));
    assert(a733_clock_release(&registry,&old,10)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&registry,5,0,A733_CLOCK_TWI11,10,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&registry,5,0,A733_CLOCK_VO1,10,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&registry,5,0,A733_CLOCK_PLL_NPU,10,&out)==A733_CLOCK_BUSY);
    struct a733_clock_token held=next;
    assert(a733_clock_retain(&registry,&next)==A733_CLOCK_FAULT);
    assert(atomic_load_explicit(&registry.failed,memory_order_acquire)==1);
    before=observations;
    assert(a733_clock_release(&registry,&next,10)==A733_CLOCK_FAULT);
    assert(a733_clock_acquire(&registry,5,0,A733_CLOCK_TWI11,10,&out)==A733_CLOCK_FAULT);
    same(&out,&saved);
    assert(observations==before);
    unsigned occupied=0;
    for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i)if(registry.slots[i].id) {
        same(&registry.slots[i],&held);++occupied;
    }
    assert(occupied==1);
    puts("PASS: independent TWI11/TWI9/VO1 scopes, FABRIC conflicts, upper-half authentication, publication and retained fault");
}
