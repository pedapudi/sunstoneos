/* SPDX-License-Identifier: Apache-2.0 */
/* Real storage/channel/MMC/PMIC composition; only hardware, time and transport
 * completions are modeled. No inherited platform scenario is executed. */
#include "a733_maintenance_watchdog_internal.h"
#include "a733_storage_update_native.h"
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "storage-service-model.h"

static struct a733_counter_clock clock_owner;
static struct a733_platform_locks locks_owner;
static struct a733_storage_native_transport transport;
static struct a733_maintenance_transfer transfer;
static struct a733_storage_grant grant;
static struct a733_maintenance_lease *lease;
static unsigned feeds;
static unsigned revoke_mode;
static void *task;
static unsigned usb_owner,storage_owner;
int a733_counter_clock_now(void *v,uint64_t *out)
{assert(v==&clock_owner);*out=m.power.now;return 1;}
int a733_maintenance_hw_admit(const struct a733_maintenance_transfer *t)
{
    if(revoke_mode==2)a733_maintenance_revoke(a733_maintenance_native_lease());
    return t==&transfer;
}
int a733_maintenance_hw_task(void *owner){return owner==task;}
int a733_maintenance_hw_reload(const struct a733_maintenance_transfer *t,
    struct a733_counter_clock *c,struct a733_platform_locks *locks,void *owner,
    atomic_uint *state,uint64_t deadline,uint64_t *before,uint64_t *after)
{
    assert(t->generation==transfer.generation&&c==&clock_owner&&locks==&locks_owner&&owner==task);
    assert(task==&usb_owner&&m.power.now<deadline&&atomic_load(state)==1);
    *before=*after=m.power.now;++feeds;return 1;
}
static int usb_poll(void)
{task=&usb_owner;return a733_maintenance_poll(lease);}
static int observe(void)
{task=&storage_owner;return a733_maintenance_storage_observe(lease,&service);}
static void number(uint8_t *p,uint64_t n,unsigned bytes)
{for(unsigned i=0;i<bytes;++i)p[i]=(uint8_t)(n>>(i*8));}
static void submit(unsigned op,uint64_t offset)
{
    uint8_t request[88]={0},hash[32];memcpy(request,grant.nonce,32);
    unsigned seq=service.sequence+1;number(request+32,seq,4);number(request+36,op,4);
    number(request+40,offset,8);memset(request+56,0x87,32);
    assert(a733_storage_channel_submit(&channel,request,sizeof(request)));
    storage_digest_bytes(request,sizeof(request),hash);
    atomic_fetch_add(&transport.version,1);
    atomic_store(&transport.accepted_sequence,seq);atomic_store(&transport.accepted_command,op);
    for(unsigned i=0;i<4;++i){uint64_t word;memcpy(&word,hash+i*8,8);atomic_store(&transport.accepted_hash[i],word);}
    atomic_fetch_add(&transport.version,1);
}
static void finish_worker(void)
{
    for(unsigned i=0;i<2000&&atomic_load(&channel.state)!=3;++i){progress();assert(observe());}
    assert(atomic_load(&channel.state)==3);
}
static void take_response(unsigned complete)
{
    uint8_t response[A733_STORAGE_BODY];size_t size;
    assert(a733_storage_channel_response(&channel,response,sizeof(response),&size));
    assert(size>=56&&response[0]==service.sequence&&response[4]==0);
    atomic_fetch_add(&transport.version,1);
    atomic_store(&transport.response_sequence,service.sequence);
    if(complete)atomic_store(&transport.completed_sequence,service.sequence);
    atomic_fetch_add(&transport.version,1);
}
static void setup(unsigned short_session)
{
    storage_setup_selected(SUNXI_MMC_SD,A733_MMC_EMMC_SDC2);
    field(m.card.csd,84,12,21);field(m.card.csd,22,4,9);ready_storage();
    for(unsigned i=0;i<sizeof(media);++i)media[i]=(uint8_t)i;
    grant=(struct a733_storage_grant){.kind=SUNXI_MMC_SD,.sectors=m.platform.core.card.sectors,
        .first=2,.blocks=2,.deadline_us=9000000};
    if(short_session)grant.deadline_us=m.power.now+2000000;
    memcpy(grant.cid,m.platform.core.card.cid,16);memset(grant.nonce,1,32);
    memset(grant.allocation,0xa1,32);memset(grant.recovery,0xb2,32);
    a733_storage_channel_init(&channel);
    transport=(struct a733_storage_native_transport){.channel=&channel,.service=&service,.owner_task=&usb_owner};
    atomic_init(&transport.version,0);atomic_init(&transport.generation,9);
    atomic_init(&transport.connected,1);
    clock_owner.initialized=1;clock_owner.frequency=24000000;
    locks_owner.initialized=1;locks_owner.now=a733_counter_clock_now;locks_owner.context=&clock_owner;
    transfer=(struct a733_maintenance_transfer){.magic=A733_MAINTENANCE_TRANSFER_MAGIC,
        .epoch=8,.generation=9,.lease=10,.storage=&grant,.reload_before_us=m.power.now,
        .reload_after_us=m.power.now,.minimum_reload_us=3800000,.maximum_recovery_us=4500000,
        .reset_by_us=14000000,.guard_us=100000};
    memset(transfer.image,0x51,32);memcpy(transfer.qualification,grant.recovery,32);
    lease=a733_maintenance_native_lease();
    if(revoke_mode==1)a733_maintenance_revoke(lease);
    int bound=a733_maintenance_bind(lease,&transfer,&clock_owner,&locks_owner,&transport,&service,&storage_owner,&usb_owner);
    if(revoke_mode) {
        assert(!bound&&!feeds&&!a733_maintenance_alive(lease));
        assert(!a733_maintenance_bind(lease,&transfer,&clock_owner,&locks_owner,&transport,&service,&storage_owner,&usb_owner));
        assert(!a733_maintenance_alive(lease)&&!feeds);return;
    }
    assert(bound);
    assert(a733_maintenance_alive(lease)&&!feeds);
    assert(a733_storage_update_init(&service,&m.platform,&grant,1000000));assert(observe());
}
int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"revoke-before-bind"))revoke_mode=1;
    else if(!strcmp(argv[1],"revoke-during-bind"))revoke_mode=2;
    setup(!strcmp(argv[1],"cutoff-guard"));
    if(revoke_mode){printf("maintenance %s passed; binding never revived lease\n",argv[1]);return 0;}
    if(!strcmp(argv[1],"cutoff-guard")) {
        /* The short fixed session cutoff is the smaller bound. The immutable
         * fixture grant remains unchanged after bind. */
        submit(A733_STORAGE_BACKUP,0);finish_worker();take_response(1);
        submit(A733_STORAGE_SEAL,2);finish_worker();assert(usb_poll()&&feeds==1);
        m.power.now=grant.deadline_us-transfer.guard_us-1;
        assert(a733_maintenance_alive(lease));++m.power.now;
        assert(!a733_maintenance_alive(lease)&&!usb_poll()&&feeds==1);
        assert(m.platform.update==&service);puts("maintenance cutoff guard passed; backing retained");return 0;
    }
    if(!strcmp(argv[1],"pending-deadline")) {
        submit(A733_STORAGE_BACKUP,0);progress();assert(observe());assert(usb_poll()&&!feeds);
        m.power.now=service.operation_deadline;
        assert(!usb_poll()&&!a733_maintenance_alive(lease)&&!feeds);
        assert(m.platform.update==&service&&service.state!=A733_STORAGE_IDLE);
    } else if(!strcmp(argv[1],"loss")) {
        atomic_store(&transport.lost,1);assert(!usb_poll()&&!feeds);
        atomic_store(&transport.lost,0);assert(!a733_maintenance_alive(lease));
        assert(m.platform.update==&service);
    } else {
        assert(!strcmp(argv[1],"completion-and-final-drain"));
        submit(A733_STORAGE_BACKUP,0);finish_worker();assert(usb_poll()&&!feeds);
        take_response(0);assert(usb_poll()&&!feeds);
        m.power.now+=100000;atomic_store(&transport.completed_sequence,service.sequence);
        assert(usb_poll()&&!feeds); /* Actual IN alone cannot renew. */
        submit(A733_STORAGE_SEAL,2);assert(usb_poll()&&!feeds); /* Storage has not admitted it. */
        finish_worker();assert(usb_poll()&&feeds==1);assert(usb_poll()&&feeds==1);
        take_response(1);submit(A733_STORAGE_INFO,0);finish_worker();take_response(1);
        assert(usb_poll()&&feeds==1); /* INFO and SEAL never create another permit. */
        submit(A733_STORAGE_FINISH,2);finish_worker();assert(service.finished);
        assert(usb_poll()&&feeds==1&&a733_maintenance_alive(lease));take_response(1);
        assert(usb_poll()&&feeds==1&&m.platform.update==&service);
        m.power.now+=3800000;assert(!usb_poll()&&!a733_maintenance_alive(lease)&&feeds==1);
    }
    printf("maintenance %s passed; backing retained\n",argv[1]);return 0;
}
