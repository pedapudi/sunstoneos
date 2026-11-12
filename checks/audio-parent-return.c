/* The PLL write ordering is the pinned vendor ccu_pll_gate_helper_disable
 * sequence (docs/i2s-platform.md). Synthetic time tests refusal, not latency. */
#define main i2s_existing_main
#include "i2s-platform.c"
#undef main
#include "a733_audio_leases.h"

static void retired(void)
{
    enum a733_i2s_resource_result r=PENDING;
    for(unsigned i=0;i<16&&r==PENDING;++i){r=a733_audio_parent_retire(&parent,900);now+=1;}
    assert(r==READY&&parent.retired&&!parent.audio.id&&!parent.reference.id&&!parent.published.id);
    assert(!a733_audio_parent_ready(&parent));
    unsigned before=reads+writes;now=1000;
    assert(a733_audio_parent_retire(&parent,900)==READY&&reads+writes==before);
    assert(a733_audio_parent_poll(&parent)==RETAINED&&reads+writes==before);
}
static void parent_cases(void)
{
    for(unsigned ldo=0;ldo<2;++ldo)for(unsigned stage=0;stage<8;++stage){
        setup(1);ccu[0x280/4]|=ldo?B(30):0;
        const uint32_t original=ccu[0x280/4];
        for(unsigned i=0;i<32&&parent.stage<stage;++i){
            enum a733_i2s_resource_result r=a733_audio_parent_poll(&parent);
            assert(r==PENDING||r==READY);now+=10;
        }
        assert(parent.stage==stage);
        unsigned start=pll_count;
        retired();
        assert((ccu[0x280/4]&~B(28))==original);
        assert(!held[A733_AUDIO_REFERENCE]&&!held[A733_AUDIO_STABLE]&&!held[A733_AUDIO_CHANGE]);
        if(stage<=2)assert(!writes);
        unsigned seen_off=0;
        for(unsigned i=start;i<pll_count;++i){
            uint32_t v=pll_trace[i];
            if(!(v&B(27)))seen_off|=1;
            if(!(v&B(29))){assert(seen_off&1);seen_off|=2;}
            if(!(v&B(31))){assert(seen_off&2);seen_off|=4;}
            if(!(v&B(30)))assert(seen_off&4);
        }
    }
    setup(0);ready_parent();unsigned before=writes;uint32_t original=ccu[0x280/4];
    retired();assert(writes==before&&ccu[0x280/4]==original);
    setup(1);ready_parent();conflict_scope=A733_AUDIO_CHANGE;before=reads+writes;
    assert(a733_audio_parent_retire(&parent,900)==PENDING&&reads+writes==before&&held[A733_AUDIO_STABLE]);
    now=900;assert(a733_audio_parent_retire(&parent,900)==RETAINED&&held[A733_AUDIO_STABLE]&&reads+writes==before);
    for(unsigned late=0;late<2;++late){
        setup(1);ready_parent();failure_address=C+0x280;write_failure=!late;late_write=late;
        assert(a733_audio_parent_retire(&parent,900)==PENDING);
        assert(a733_audio_parent_retire(&parent,900)==RETAINED&&parent.audio.id&&parent.reference.id);
        before=reads+writes;assert(a733_audio_parent_retire(&parent,900)==RETAINED&&reads+writes==before);
    }
    setup(1);ready_parent();ccu[0x1210/4]|=B(31);before=writes;
    assert(a733_audio_parent_retire(&parent,900)==PENDING);
    assert(a733_audio_parent_retire(&parent,900)==RETAINED&&writes==before&&parent.audio.id);
    setup(1);ready_parent();late_publish=1;before=writes;
    assert(a733_audio_parent_retire(&parent,900)==RETAINED&&parent.audio.id&&parent.published.id&&writes==before);
    setup(0);ready_parent();late_release=1;
    assert(a733_audio_parent_retire(&parent,900)==PENDING);
    assert(a733_audio_parent_retire(&parent,900)==RETAINED&&parent.audio.id&&parent.reference.id);
    puts("PASS audio parent return: 16 partial cold states, inherited LDO, adopted no-write release, order, conflict, uncertain/late write and token retention");
}
static int registry_now(void *cookie,uint64_t *out){*out=*(uint64_t *)cookie;return 1;}
static void registry_cases(void)
{
    struct a733_clock_leases registry={0};uint64_t time=1;
    struct a733_clock_token change={0},stable={0},other={0},out={0};
    assert(a733_clock_leases_init(&registry,registry_now,&time));
    assert(a733_clock_acquire(&registry,1,0,A733_CLOCK_AUDIO_CHANGE,900,&change)==A733_CLOCK_OK);
    assert(a733_clock_publish_audio(&registry,&change,900,&stable)==A733_CLOCK_OK);
    const a733_clock_mask_t conflicts[]={A733_CLOCK_AUDIO1,A733_CLOCK_AUDIO2,A733_CLOCK_AUDIO5,A733_CLOCK_I2S0,A733_CLOCK_DMIC};
    for(unsigned i=0;i<sizeof(conflicts)/sizeof(*conflicts);++i){
        a733_clock_mask_t exclusive=i>=3?conflicts[i]:0,shared=i<3?conflicts[i]:0;
        assert(a733_clock_acquire(&registry,2,shared,exclusive,900,&other)==A733_CLOCK_OK);
        assert(a733_clock_reclaim_audio(&registry,&stable,900,&out)==A733_CLOCK_BUSY&&!out.id);
        assert(a733_clock_token_status(&registry,&stable)==A733_CLOCK_OK);
        assert(a733_clock_release(&registry,&other,900)==A733_CLOCK_OK);
    }
    struct a733_clock_token wrong=stable;wrong.owner++;
    assert(a733_clock_reclaim_audio(&registry,&wrong,900,&out)==A733_CLOCK_INVALID&&!out.id);
    assert(a733_clock_reclaim_audio(&registry,&stable,1,&out)==A733_CLOCK_EXPIRED&&!out.id);
    assert(a733_clock_reclaim_audio(&registry,&stable,900,&out)==A733_CLOCK_OK&&out.id!=stable.id&&
           out.exclusive==A733_CLOCK_AUDIO_CHANGE&&out.shared==(A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(a733_clock_token_status(&registry,&stable)==A733_CLOCK_INVALID);
    assert(a733_clock_reclaim_audio(&registry,&stable,900,&change)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&registry,2,A733_CLOCK_AUDIO2,0,900,&other)==A733_CLOCK_BUSY);
    puts("PASS audio clock reverse publication: all consumer classes, exact identity, stale/expired refusal and exclusive exclusion");
}
static void adapter_cases(void)
{
    struct a733_clock_leases registry={0};uint64_t time=1;
    struct a733_audio_leases leases={0};
    struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=time_now,.read=read_word,.write=write_word};
    struct a733_audio_token reference={0},change={0},stable={0},local={0},replacement={0};
    struct a733_clock_token other={0};
    assert(a733_clock_leases_init(&registry,registry_now,&time));
    assert(a733_audio_leases_init(&leases,&registry,1,&backend,&parent));
    assert(leases.operations.acquire(&leases,A733_AUDIO_REFERENCE,&reference,900)==READY);
    assert(leases.operations.acquire(&leases,A733_AUDIO_CHANGE,&change,900)==READY);
    assert(leases.operations.publish(&leases,&change,&stable,900)==READY);
    assert(leases.operations.acquire(&leases,A733_AUDIO_LOCAL,&local,900)==READY);
    assert(leases.operations.release(&leases,&local,900)==READY);
    assert(a733_clock_acquire(&registry,2,A733_CLOCK_AUDIO2,0,900,&other)==A733_CLOCK_OK);
    assert(leases.operations.publish(&leases,&stable,&replacement,900)==BUSY&&!replacement.id&&
           leases.tokens[A733_AUDIO_STABLE-1].id==stable.id);
    assert(a733_clock_release(&registry,&other,900)==A733_CLOCK_OK);
    assert(leases.operations.publish(&leases,&stable,&replacement,900)==READY);
    assert(!leases.tokens[A733_AUDIO_STABLE-1].id&&leases.tokens[A733_AUDIO_CHANGE-1].id==replacement.id);
    assert(leases.operations.release(&leases,&replacement,900)==READY);
    assert(leases.operations.release(&leases,&reference,900)==READY);
    for(unsigned i=0;i<A733_CLOCK_CLIENTS;++i)assert(!registry.slots[i].id);
    assert(leases.operations.release(&leases,&stable,900)==RETAINED&&leases.failed);
    puts("PASS native audio lease adapter: exact bidirectional conversion, busy consumer, local close, parent release and stale refusal");
}
int main(void){parent_cases();registry_cases();adapter_cases();return 0;}
