#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int now(void *cookie,uint64_t *out)
{(void)cookie;*out=1;return 1;}
int main(void)
{
    _Static_assert(sizeof(a733_clock_mask_t)==8,"64-bit masks");
    _Static_assert(sizeof(struct a733_clock_token)==32,"token ABI unchanged");
    _Static_assert(A733_CLOCK_SDC1==(UINT64_C(1)<<53),"existing SDC1 value");
    _Static_assert(A733_CLOCK_SDC0==(UINT64_C(1)<<56),"SDC0 allocation");
    _Static_assert(A733_CLOCK_SDC2==(UINT64_C(1)<<57),"SDC2 allocation");
    _Static_assert(A733_CLOCK_SDC3==(UINT64_C(1)<<58),"SDC3 allocation");
    struct a733_clock_leases r={0};
    assert(a733_clock_leases_init(&r,now,NULL));
    const a733_clock_mask_t local[]={A733_CLOCK_SDC0,A733_CLOCK_SDC1,A733_CLOCK_SDC2,A733_CLOCK_SDC3};
    const a733_clock_mask_t parent=A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC;
    struct a733_clock_token live[4],out={.id=999},saved=out;
    for(unsigned i=0;i<4;++i) {
        assert(a733_clock_acquire(&r,i+1,A733_CLOCK_REF|A733_CLOCK_FABRIC,local[i],10,&live[i])==A733_CLOCK_OK);
        assert(live[i].exclusive==local[i] && live[i].shared==parent);
        assert(a733_clock_acquire(&r,9,0,local[i],10,&out)==A733_CLOCK_BUSY);
        assert(!memcmp(&out,&saved,sizeof(out)));
        assert(a733_clock_acquire(&r,9,local[i],0,10,&out)==A733_CLOCK_INVALID);
        assert(!memcmp(&out,&saved,sizeof(out)));
    }
    const a733_clock_mask_t writers[]={A733_CLOCK_REF,A733_CLOCK_OSC,A733_CLOCK_FABRIC};
    for(unsigned i=0;i<3;++i)
        assert(a733_clock_acquire(&r,9,0,writers[i],10,&out)==A733_CLOCK_BUSY);
    struct a733_clock_token forged=live[0];forged.exclusive=A733_CLOCK_SDC2;
    assert(a733_clock_release(&r,&forged,10)==A733_CLOCK_INVALID);
    assert(a733_clock_token_status(&r,&live[0])==A733_CLOCK_OK);
    assert(a733_clock_release(&r,&live[0],10)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&r,&live[0])==A733_CLOCK_INVALID);
    /* Local ownership alone adds no guessed module/fabric parent. */
    assert(a733_clock_acquire(&r,8,0,A733_CLOCK_SDC0,10,&out)==A733_CLOCK_OK);
    assert(!out.shared && out.exclusive==A733_CLOCK_SDC0);
    assert(a733_clock_release(&r,&out,10)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&r,&live[1])==A733_CLOCK_OK);
    assert(a733_clock_token_status(&r,&live[2])==A733_CLOCK_OK);
    assert(a733_clock_token_status(&r,&live[3])==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,9,0,UINT64_C(1)<<59,10,&out)==A733_CLOCK_INVALID);
    assert(a733_clock_release(&r,&live[2],10)==A733_CLOCK_OK);
    assert(a733_clock_release(&r,&live[3],10)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,8,A733_CLOCK_REF|A733_CLOCK_FABRIC,
        A733_CLOCK_SDC2|A733_CLOCK_SDC3,10,&out)==A733_CLOCK_OK);
    struct a733_clock_token collision={0};
    for(unsigned i=2;i<4;++i)
        assert(a733_clock_acquire(&r,9,0,local[i],10,&collision)==A733_CLOCK_BUSY);
    assert(!collision.id&&a733_clock_token_status(&r,&live[1])==A733_CLOCK_OK);
    puts("PASS: SDC0/1/2/3 scopes, joint eMMC exclusion, explicit parents and unchanged token ABI");
}
