#include "a733_audio_leases.h"
#include <pthread.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

/* Reuse the existing register/pin model, not its old resource callbacks or
 * broad test entry. Only new lease composition cases execute below. */
#define main existing_i2s_platform_checks
#include "i2s-platform.c"
#undef main

static struct a733_clock_leases registry;
static struct a733_audio_leases leases;
static pthread_barrier_t race_start;
static struct a733_clock_token raced[2];
static enum a733_clock_result race_result[2];
static void registry_init(void)
{
    memset(&registry,0,sizeof(registry));
    assert(a733_clock_leases_init(&registry,time_now,&parent));
}
static int composed_write(void *ctx,uintptr_t address,uint32_t value)
{
    held[A733_AUDIO_CHANGE]=!!leases.tokens[A733_AUDIO_CHANGE-1].id;
    held[A733_AUDIO_LOCAL]=!!leases.tokens[A733_AUDIO_LOCAL-1].id;
    return write_word(ctx,address,value);
}
static void composed_setup(unsigned cold)
{
    setup(cold);registry_init();memset(&leases,0,sizeof(leases));
    struct a733_audio_operations backend=parent.ops;
    backend.write=composed_write;
    assert(a733_audio_leases_init(&leases,&registry,100,&backend,&parent));
    parent.ops=leases.operations;parent.cookie=&leases;
}
/* The registry holds the published stable AUDIO5 profile. */
static int stable_audio(void)
{
    const struct a733_clock_token *t=&leases.tokens[A733_AUDIO_STABLE-1];
    return a733_clock_token_status(&registry,t)==A733_CLOCK_OK &&
        !t->exclusive && (t->shared&A733_CLOCK_AUDIO5);
}
static unsigned held_count(void)
{unsigned count=0;for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i)count+=registry.slots[i].id!=0;return count;}
static void *race(void *arg)
{
    unsigned i=(unsigned)(uintptr_t)arg;
    (void)pthread_barrier_wait(&race_start);
    race_result[i]=a733_clock_acquire(&registry,i+1,0,A733_CLOCK_I2S0,1000,&raced[i]);
    return NULL;
}
static void ownership_cases(void)
{
    setup(0);registry_init();
    struct a733_clock_token gpu={0},audio={0},other={.id=777},saved=other;
    assert(a733_clock_acquire(&registry,1,A733_CLOCK_PERI0,0,1000,&gpu)==A733_CLOCK_OK);
    assert(gpu.shared==(A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_acquire(&registry,2,0,A733_CLOCK_REF,1000,&other)==A733_CLOCK_BUSY);
    assert(!memcmp(&other,&saved,sizeof(other)));
    assert(a733_clock_acquire(&registry,2,0,A733_CLOCK_AUDIO_CHANGE,1000,&audio)==A733_CLOCK_OK);
    assert(audio.shared==(A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_I2S4,1000,&other)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&registry,3,A733_CLOCK_AUDIO2,0,1000,&other)==A733_CLOCK_BUSY);
    struct a733_clock_token stable={0};
    assert(a733_clock_publish_audio(&registry,&audio,1000,&stable)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&registry,&stable)==A733_CLOCK_OK && stable.id!=audio.id && held_count()==2);
    assert(a733_clock_release(&registry,&audio,1000)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&registry,3,0,A733_CLOCK_I2S4,1000,&other)==A733_CLOCK_OK);
    struct a733_clock_token changed=stable;changed.shared^=A733_CLOCK_AUDIO5;
    assert(a733_clock_release(&registry,&changed,1000)==A733_CLOCK_INVALID);
    now=1000;assert(a733_clock_release(&registry,&stable,1000)==A733_CLOCK_EXPIRED && held_count()==3);
    now=1001;assert(a733_clock_release(&registry,&stable,2000)==A733_CLOCK_OK);
    assert(a733_clock_release(&registry,&other,2000)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&registry,2,0,A733_CLOCK_AUDIO_CHANGE,2000,&audio)==A733_CLOCK_OK);
    assert(a733_clock_token_status(&registry,&audio)==A733_CLOCK_OK && audio.exclusive==A733_CLOCK_AUDIO_CHANGE);
    assert(a733_clock_retain(&registry,&audio)==A733_CLOCK_FAULT && held_count()==2);
    assert(a733_clock_release(&registry,&gpu,2000)==A733_CLOCK_FAULT);
    assert(a733_clock_acquire(&registry,3,A733_CLOCK_OSC,0,2000,&other)==A733_CLOCK_FAULT);

    setup(0);registry_init();
    assert(a733_clock_acquire(&registry,1,A733_CLOCK_I2S0,0,1000,&other)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&registry,1,A733_CLOCK_REF,A733_CLOCK_REF,1000,&other)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&registry,1,A733_CLOCK_REF,0,1000,&registry.slots[0])==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&registry,1,A733_CLOCK_REF,0,1,&other)==A733_CLOCK_EXPIRED);
    assert(held_count()==0);
    registry.next_id=UINT64_MAX;
    assert(a733_clock_acquire(&registry,1,A733_CLOCK_REF,0,1000,&gpu)==A733_CLOCK_OK && gpu.id==UINT64_MAX);
    assert(a733_clock_acquire(&registry,2,A733_CLOCK_REF,0,1000,&audio)==A733_CLOCK_EXHAUSTED);
    assert(a733_clock_release(&registry,&gpu,1000)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&registry,2,A733_CLOCK_REF,0,1000,&audio)==A733_CLOCK_EXHAUSTED);

    setup(0);registry_init();
    for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i)
        assert(a733_clock_acquire(&registry,i+1,A733_CLOCK_REF,0,1000,&other)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&registry,100,A733_CLOCK_REF,0,1000,&other)==A733_CLOCK_BUSY);
    now=0;assert(a733_clock_release(&registry,&other,1000)==A733_CLOCK_FAULT && held_count()==A733_CLOCK_CLIENTS);
}
static void race_case(void)
{
    setup(0);registry_init();memset(raced,0,sizeof(raced));
    assert(!pthread_barrier_init(&race_start,NULL,2));
    pthread_t threads[2];
    for(unsigned i=0;i<2;++i)assert(!pthread_create(&threads[i],NULL,race,(void *)(uintptr_t)i));
    for(unsigned i=0;i<2;++i)assert(!pthread_join(threads[i],NULL));
    assert((race_result[0]==A733_CLOCK_OK && race_result[1]==A733_CLOCK_BUSY) ||
           (race_result[1]==A733_CLOCK_OK && race_result[0]==A733_CLOCK_BUSY));
    assert(held_count()==1);assert(!pthread_barrier_destroy(&race_start));
}
static void composition_cases(void)
{
    composed_setup(1);
    struct a733_clock_token gpu={0},other={0};
    assert(a733_clock_acquire(&registry,200,A733_CLOCK_PERI0,0,900,&gpu)==A733_CLOCK_OK);
    ready_parent();assert(stable_audio() && held_count()==3);
    assert(parent.audio.id==leases.tokens[A733_AUDIO_STABLE-1].id);
    assert(!leases.tokens[A733_AUDIO_CHANGE-1].id);
    assert(a733_clock_acquire(&registry,201,0,A733_CLOCK_REF,900,&other)==A733_CLOCK_BUSY);
    ready_local();assert(held_count()==4);
    assert(a733_clock_acquire(&registry,201,0,A733_CLOCK_I2S0,900,&other)==A733_CLOCK_BUSY);
    enum a733_i2s_resource_result r=iops.release(&binding,900);
    for(unsigned i=0;i<4 && r==PENDING;++i)r=iops.release(&binding,900);
    assert(r==READY && held_count()==3 && parent.audio.id && parent.reference.id);
    assert(ccu[0x1200/4]==B(12) && ccu[0x120c/4]==B(8));
    ready_local();assert(held_count()==4);

    composed_setup(1);
    assert(a733_clock_acquire(&registry,200,0,A733_CLOCK_I2S1,900,&other)==A733_CLOCK_OK);
    assert(a733_audio_parent_poll(&parent)==PENDING);
    assert(a733_audio_parent_poll(&parent)==PENDING && !parent.audio.id && !writes && !reads);
    assert(a733_clock_release(&registry,&other,900)==A733_CLOCK_OK);
    ready_parent();assert(stable_audio());

    composed_setup(0);ready_parent();assert(writes==0 && held_count()==2);
    ready_local();write_failure=1;failure_address=C+0x1200;
    assert(iops.release(&binding,900)==RETAINED && held_count()==3 && binding.local.id);
    failed_local_stays_quiet();
    assert(a733_clock_acquire(&registry,200,0,A733_CLOCK_I2S0,900,&other)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&registry,200,0,A733_CLOCK_AUDIO1,900,&other)==A733_CLOCK_BUSY);
}
int main(void)
{
    ownership_cases();race_case();composition_cases();
    puts("PASS common clock leases: parent conflicts, atomic publication, authentication, concurrency, deadlines and real I2S binding");
    return 0;
}
