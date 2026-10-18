#define _POSIX_C_SOURCE 200809L
#include "a733_clock_leases.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static int now(void *context,uint64_t *out)
{(void)context;*out=1;return 1;}
static void fresh(struct a733_clock_leases *r)
{memset(r,0,sizeof(*r));assert(a733_clock_leases_init(r,now,0));}
static const a733_clock_mask_t locals[]={
    A733_CLOCK_UART0,A733_CLOCK_UART1,A733_CLOCK_UART2,A733_CLOCK_UART3,
    A733_CLOCK_UART4,A733_CLOCK_UART5,A733_CLOCK_UART6,A733_CLOCK_S_UART0,
    A733_CLOCK_S_UART1,A733_CLOCK_SPI0,A733_CLOCK_SPI1,A733_CLOCK_SPI2,
    A733_CLOCK_SPI3,A733_CLOCK_SPI4,A733_CLOCK_S_SPI,A733_CLOCK_CE};

static void coexistence(void)
{
    struct a733_clock_leases r;fresh(&r);
    struct a733_clock_token live[sizeof(locals)/sizeof(*locals)],out={.id=999};
    for(unsigned i=0;i<sizeof(live)/sizeof(*live);++i) {
        a733_clock_mask_t shared=i<9 ? 0 : A733_CLOCK_PERI0|A733_CLOCK_FABRIC;
        if(locals[i]==A733_CLOCK_CE)shared|=A733_CLOCK_RC16M;
        assert(a733_clock_acquire(&r,i+1,shared,locals[i],10,&live[i])==A733_CLOCK_OK);
        assert(live[i].exclusive==locals[i]);
        if(i<9)assert(live[i].shared==((i<7 ? A733_CLOCK_APB_UART : A733_CLOCK_APBS1)|
            A733_CLOCK_REF|A733_CLOCK_OSC));
        assert(a733_clock_acquire(&r,99,0,locals[i],10,&out)==A733_CLOCK_BUSY && out.id==999);
        assert(a733_clock_acquire(&r,99,locals[i],0,10,&out)==A733_CLOCK_INVALID && out.id==999);
    }
    const a733_clock_mask_t parents[]={A733_CLOCK_APB_UART,A733_CLOCK_APBS1,
        A733_CLOCK_REF,A733_CLOCK_OSC,A733_CLOCK_PERI0,A733_CLOCK_FABRIC,A733_CLOCK_RC16M};
    for(unsigned i=0;i<sizeof(parents)/sizeof(*parents);++i)
        assert(a733_clock_acquire(&r,99,0,parents[i],10,&out)==A733_CLOCK_BUSY && out.id==999);
    /* A token with only its upper mask changed cannot authenticate. */
    struct a733_clock_token forged=live[0];forged.exclusive=A733_CLOCK_UART1;
    assert(a733_clock_token_status(&r,&forged)==A733_CLOCK_INVALID);
    assert(a733_clock_release(&r,&forged,10)==A733_CLOCK_INVALID);
    assert(a733_clock_token_status(&r,&live[0])==A733_CLOCK_OK);
    for(unsigned i=0;i<sizeof(live)/sizeof(*live);++i)
        assert(a733_clock_release(&r,&live[i],10)==A733_CLOCK_OK);
    for(unsigned bit=0;bit<128;++bit) {
        if(A733_CLOCK_ALL&(A733_CLOCK_BIT(bit)))continue;
        assert(a733_clock_acquire(&r,99,A733_CLOCK_BIT(bit),0,10,&out)==A733_CLOCK_INVALID && out.id==999);
        assert(a733_clock_acquire(&r,99,0,A733_CLOCK_BIT(bit),10,&out)==A733_CLOCK_INVALID && out.id==999);
    }
    assert(a733_clock_acquire(&r,1,A733_CLOCK_RC16M,0,10,&live[0])==A733_CLOCK_OK);
    assert(live[0].shared==A733_CLOCK_RC16M && !live[0].exclusive);
    assert(a733_clock_acquire(&r,2,0,A733_CLOCK_OSC,10,&live[1])==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,3,0,A733_CLOCK_RC16M,10,&out)==A733_CLOCK_BUSY);
}
static void publication(void)
{
    struct a733_clock_leases r;fresh(&r);
    struct a733_clock_token old,next={.id=999},saved=next;
    const a733_clock_mask_t locals=A733_CLOCK_S_UART1|A733_CLOCK_CE|A733_CLOCK_NPU;
    assert(a733_clock_acquire(&r,1,A733_CLOCK_RC16M,
        A733_CLOCK_PLL_NPU|locals,10,&old)==A733_CLOCK_OK);
    assert(a733_clock_publish_stable(&r,&old,A733_CLOCK_PLL_NPU,1,&next)==A733_CLOCK_EXPIRED);
    assert(!memcmp(&next,&saved,sizeof(next)) && a733_clock_token_status(&r,&old)==A733_CLOCK_OK);
    assert(a733_clock_publish_stable(&r,&old,A733_CLOCK_PLL_NPU,10,&old)==A733_CLOCK_INVALID);
    assert(a733_clock_publish_stable(&r,&old,A733_CLOCK_RC16M,10,&next)==A733_CLOCK_INVALID);
    assert(a733_clock_publish_stable(&r,&old,A733_CLOCK_PLL_NPU,10,&next)==A733_CLOCK_OK);
    assert(next.id!=old.id && next.owner==old.owner && next.exclusive==locals);
    assert(next.shared==(old.shared|A733_CLOCK_PLL_NPU));
    assert(a733_clock_token_status(&r,&old)==A733_CLOCK_INVALID);
    assert(a733_clock_release(&r,&old,10)==A733_CLOCK_INVALID);
    assert(a733_clock_token_status(&r,&next)==A733_CLOCK_OK);
    struct a733_clock_token out={.id=999};
    assert(a733_clock_acquire(&r,2,0,A733_CLOCK_CE,10,&out)==A733_CLOCK_BUSY);
    assert(a733_clock_release(&r,&next,10)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,2,0,A733_CLOCK_CE,10,&out)==A733_CLOCK_OK);
}
static void compatibility_and_capacity(void)
{
    struct a733_clock_leases r;fresh(&r);
    struct a733_clock_token t,u;
    _Static_assert(A733_CLOCK_LOCALS==0x3fc0 && A733_CLOCK_AUDIO_CHANGE==0x3ff8,"audio scopes retain values");
    _Static_assert(sizeof(a733_clock_mask_t)==16 && sizeof(struct a733_clock_token)==48,"private token width");
    assert(a733_clock_acquire(&r,1,0,A733_CLOCK_AUDIO_CHANGE,10,&t)==A733_CLOCK_OK);
    assert(t.shared==(A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_publish_audio(&r,&t,10,&u)==A733_CLOCK_OK && r.audio_profile==1);
    assert(u.shared==(A733_CLOCK_AUDIO5|A733_CLOCK_AUDIO1|A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_release(&r,&u,10)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,1,A733_CLOCK_PERI0,A733_CLOCK_DMA0,10,&t)==A733_CLOCK_OK);
    assert(t.shared==(A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_release(&r,&t,10)==A733_CLOCK_OK);
    struct a733_clock_token slots[A733_CLOCK_CLIENTS],out={.id=999};
    for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i)
        assert(a733_clock_acquire(&r,i+1,A733_CLOCK_RC16M,0,10,&slots[i])==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,999,A733_CLOCK_RC16M,0,10,&out)==A733_CLOCK_BUSY && out.id==999);
    assert(a733_clock_release(&r,&slots[A733_CLOCK_CLIENTS-1],10)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&r,999,A733_CLOCK_RC16M,0,10,&out)==A733_CLOCK_OK);
    assert(out.id!=slots[A733_CLOCK_CLIENTS-1].id);
    assert(a733_clock_token_status(&r,&slots[A733_CLOCK_CLIENTS-1])==A733_CLOCK_INVALID);
}
struct race {
    struct a733_clock_leases registry;
    pthread_barrier_t barrier;
    struct a733_clock_token tokens[2];
    enum a733_clock_result result[2];
};
struct racer {struct race *race;unsigned index;};
static void *contender(void *context)
{
    struct racer *c=context;struct race *r=c->race;
    int b=pthread_barrier_wait(&r->barrier);assert(b==0 || b==PTHREAD_BARRIER_SERIAL_THREAD);
    r->result[c->index]=a733_clock_acquire(&r->registry,c->index+1,0,A733_CLOCK_S_SPI,10,&r->tokens[c->index]);
    return 0;
}
static void high_bit_race(void)
{
    struct race r={0};fresh(&r.registry);assert(!pthread_barrier_init(&r.barrier,0,2));
    struct racer args[2]={{&r,0},{&r,1}};pthread_t tasks[2];
    for(unsigned i=0;i<2;++i)assert(!pthread_create(&tasks[i],0,contender,&args[i]));
    for(unsigned i=0;i<2;++i)assert(!pthread_join(tasks[i],0));
    assert((r.result[0]==A733_CLOCK_OK && r.result[1]==A733_CLOCK_BUSY) ||
        (r.result[1]==A733_CLOCK_OK && r.result[0]==A733_CLOCK_BUSY));
    unsigned winner=r.result[0]==A733_CLOCK_OK ? 0 : 1;
    assert(r.tokens[winner].exclusive==A733_CLOCK_S_SPI && !r.tokens[1-winner].id);
    assert(a733_clock_release(&r.registry,&r.tokens[winner],10)==A733_CLOCK_OK);
    assert(!pthread_barrier_destroy(&r.barrier));
}
int main(void)
{
    coexistence();publication();compatibility_and_capacity();high_bit_race();
    puts("PASS: 128-bit clock ownership, independent controllers, parents, publication, capacity and contention");
}
