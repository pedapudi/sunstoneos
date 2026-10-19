/* Real producer/consumer mailbox stress and stale identity/sequence refusals.
 * CPU threads model task scheduling; no device operations or keyboard injection. */
#include "keyboard.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
static struct mimus_keyboard_report report={.generation=23,.sequence=1,.address=3,.speed=1,
    .protocol=1,.endpoint=0x81,.subclass=1,.bytes=8};
static void *produce(void *unused)
{
    (void)unused;
    for(unsigned i=1;i<=1001;++i){
        report.sequence=i;if(i==1001)report.data[2]=0x28;
        int r;while((r=mimus_keyboard_offer(&report))==0)sched_yield();assert(r==1);
    }
    return NULL;
}
static void *consume(void *unused)
{
    (void)unused;int r;
    while((r=mimus_keyboard_step())==0)sched_yield();assert(r==1);return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);uint8_t first[128],second[128];
    if(!strcmp(argv[1],"mailbox")){
        pthread_t producer,consumer;assert(!pthread_create(&producer,NULL,produce,NULL));assert(!pthread_create(&consumer,NULL,consume,NULL));
        assert(!pthread_join(producer,NULL)&&!pthread_join(consumer,NULL));
        assert(mimus_keyboard_copy(first)==1&&mimus_keyboard_copy(second)==1&&!memcmp(first,second,128));
        assert(first[32]==(1000&255)&&first[33]==(1000>>8)&&first[80]==(1001&255)&&first[81]==(1001>>8));
        assert(!mimus_keyboard_empty()&&mimus_keyboard_offer(&report)==0);puts("PASS immutable publication and one-shot backpressure across real CPU threads");return 0;
    }
    assert(mimus_keyboard_offer(&report)==1);
    struct mimus_keyboard_report other=report;other.data[2]=0x28;
    assert(mimus_keyboard_offer(&other)==0);assert(mimus_keyboard_step()==0);
    ++report.sequence;report.data[2]=0x28;
    if(!strcmp(argv[1],"replay"))--report.sequence;
    else if(!strcmp(argv[1],"generation"))++report.generation;
    else if(!strcmp(argv[1],"address"))++report.address;
    else if(!strcmp(argv[1],"duplicate"))report.data[3]=0x28;
    else if(!strcmp(argv[1],"modifier")){
        report.data[0]=2;assert(mimus_keyboard_offer(&report)==1&&mimus_keyboard_step()==0);
        ++report.sequence;report.data[0]=0;assert(mimus_keyboard_offer(&report)==1&&mimus_keyboard_step()==0);
        assert(mimus_keyboard_copy(first)==0);puts("PASS modified Enter cannot arm or inherit neutral state");return 0;
    }else return 2;
    assert(mimus_keyboard_offer(&report)==1&&mimus_keyboard_step()==-1);
    assert(mimus_keyboard_copy(first)==-1&&!mimus_keyboard_empty()&&mimus_keyboard_offer(&report)==-1);
    puts("PASS stale or malformed report permanently refused");return 0;
}
