#include "a733_clock_leases.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int now(void *p,uint64_t *out) {(void)p;*out=10;return 1;}
int main(void)
{
    struct a733_clock_leases m={0};
    struct a733_clock_token dma={0}, other={.id=99}, saved=other;
    assert(a733_clock_leases_init(&m,now,0));
    assert(a733_clock_acquire(&m,1,A733_CLOCK_DMA0,0,20,&other)==A733_CLOCK_INVALID);
    assert(!memcmp(&other,&saved,sizeof(other)));
    assert(a733_clock_acquire(&m,1,A733_CLOCK_FABRIC|A733_CLOCK_PERI0,
        A733_CLOCK_DMA0,20,&dma)==A733_CLOCK_OK);
    assert(dma.shared==(A733_CLOCK_FABRIC|A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC));
    const a733_clock_mask_t writers[]={A733_CLOCK_DMA0,A733_CLOCK_FABRIC,A733_CLOCK_PERI0,
        A733_CLOCK_REF,A733_CLOCK_OSC};
    for(unsigned i=0;i<sizeof(writers)/sizeof(*writers);++i) {
        assert(a733_clock_acquire(&m,2,0,writers[i],20,&other)==A733_CLOCK_BUSY);
        assert(!memcmp(&other,&saved,sizeof(other)));
    }
    assert(a733_clock_acquire(&m,2,A733_CLOCK_FABRIC|A733_CLOCK_PERI0,0,20,&other)==A733_CLOCK_OK);
    assert(a733_clock_release(&m,&other,20)==A733_CLOCK_OK);
    assert(a733_clock_release(&m,&dma,20)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&m,2,0,A733_CLOCK_FABRIC,20,&other)==A733_CLOCK_OK);
    assert(A733_CLOCK_LOCALS==0x3fc0 && A733_CLOCK_AUDIO_CHANGE==0x3ff8);
    puts("DMA0 exclusive admission and fabric/parent stability conflicts pass");
}
