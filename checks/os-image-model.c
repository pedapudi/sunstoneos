/* Host model of the integrated OS image's fixed parts: interrupt delivery
 * through its SPI table, resident watchdog supervision over more than 60
 * modeled seconds, and the OS contract and session admission. It compiles
 * the image's own sources; no hardware access.
 *
 *   clang -std=c11 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *     -Iinclude -Iplatform/a733-smp-cached -Iapps/os -DSUNSTONE_BOARD_SUPERVISED=1 \
 *     -DSUNSTONE_BOARD_USB2_DUAL=1 [-DSUNSTONE_BOARD_RESIDENT=1] [-DSUNSTONE_A733_OS_AUDIO=1] \
 *     [-DSUNSTONE_A733_OS_RADIO=1] [-DSUNSTONE_A733_OS_HEADSET=1] \
 *     -DA733_SMP_CONTRACT_MAGIC=<OS magic for that lifetime> \
 *     checks/os-image-model.c drivers/storage/storage_digest.c -o model && ./model
 *
 * The CPU clock build adds -DA733_A76_ADMITTED=1 -DSUNSTONE_A733_OS_CPU_CLOCK=1
 * and drivers/clock/a733_cpu_clock.c, whose grant loader the admission calls.
 *
 * With --write-session PATH GENERATION it writes a nominal session for the
 * stage-2 check; with --admit CONTRACT SESSION NOW it reports whether the
 * image's admission accepts that pair at counter value NOW. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* Modeled GIC distributor for the image's own table and irq_table.c. */
static uint8_t gicd[0x10000] __attribute__((aligned(8)));
#define A733_IRQ_TABLE_MMIO_DEFINED 1
static uint32_t gicd_read(uint64_t o){uint32_t v;memcpy(&v,gicd+o,4);return v;}
static void gicd_write(uint64_t o,uint32_t v){memcpy(gicd+o,&v,4);}
static uint64_t gicd_read64(uint64_t o){uint64_t v;memcpy(&v,gicd+o,8);return v;}
static void gicd_write64(uint64_t o,uint64_t v){memcpy(gicd+o,&v,8);}
static void gicd_barrier(void){}
#include "../platform/a733-smp-cached/irq_table.c"
#include "../apps/os/irq.c"
static unsigned thermal_calls,audio_calls;
void os_thermal_irq(void){++thermal_calls;}
#ifdef SUNSTONE_A733_OS_AUDIO
void os_audio_irq(void){++audio_calls;}
#define ROUTES 2u
#else
#define ROUTES 1u
#endif

/* Modeled counter and WDT0 for apps/os/services.c. */
static uint64_t ticks;
uint64_t a733_smp_counter(void){return ticks;}
#define HZ UINT64_C(24000000)
static uint64_t expires,reloads,last_reload;
static int reload_fails;
int os_watchdog_reload(void *supervisor)
{
    assert(supervisor==&ticks);
    if(reload_fails||ticks>=expires)return 0;
    expires=ticks+4*HZ;++reloads;last_reload=ticks;return 1;
}
#include "../apps/os/services.c"

/* Admission compiled with this build's lifetime. checks/os-image-launch.py
 * sets OS_MODEL_BUILD_ID to a linked image's identity, so that --admit judges
 * that image's prepared session. */
#ifndef OS_MODEL_BUILD_ID
#define OS_MODEL_BUILD_ID 0x4f,0x53,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
#endif
const unsigned char a733_smp_build_id[32]={OS_MODEL_BUILD_ID};
#include "../apps/os/admission.c"

/* The DMA0 interrupt of the audio build. */
#define DMA0_INTID 143u
static int routed(unsigned id){return id==A733_THERMAL_INTID||(ROUTES==2&&id==DMA0_INTID);}
static void irq_cases(void)
{
    assert(a733_irq_table_valid()&&a733_irq_route_count==ROUTES);
    /* Each table interrupt runs its handler on CPU0 only. Every other INTID,
     * and a table INTID on another core, returns 0: the shared handler then
     * takes the status-4 stop. */
    assert(a733_irq_table_dispatch(A733_THERMAL_INTID,0)&&thermal_calls==1&&!audio_calls);
    if(ROUTES==2)assert(a733_irq_table_dispatch(DMA0_INTID,0)&&audio_calls==1&&thermal_calls==1);
    for(unsigned core=1;core<8;++core)
        assert(!a733_irq_table_dispatch(A733_THERMAL_INTID,core)&&!a733_irq_table_dispatch(DMA0_INTID,core));
    for(unsigned id=32;id<1020;++id)if(!routed(id))assert(!a733_irq_table_dispatch(id,0));
    assert(thermal_calls==1&&audio_calls==ROUTES-1);
    /* EL3 route programming: Group1NS, level, priority 0xd8, CPU0 affinity,
     * every source left disabled. */
    memset(gicd,0,sizeof(gicd));gicd_write(0,0x30);gicd_write(4,10);
    ticks=0;assert(a733_irq_table_route(HZ,a733_smp_counter));
    assert(gicd_read(0x88)&(1u<<7)&&!(gicd_read(0xd08)&(1u<<7))&&!(gicd_read(0xc10)&(3u<<14)));
    assert((gicd_read(0x444)>>24)==0xd8&&gicd_read64(0x6000+8*71)==0&&!gicd_read(0x108));
    if(ROUTES==2){
        assert(gicd_read(0x90)&(1u<<15)&&!(gicd_read(0xd10)&(1u<<15))&&!(gicd_read(0xc20)&(3u<<30)));
        assert((gicd_read(0x48c)>>24)==0xd8&&gicd_read64(0x6000+8*DMA0_INTID)==0&&!gicd_read(0x110));
    }
    /* An inherited pending table interrupt refuses the route. */
    for(unsigned i=0;i<ROUTES;++i){
        unsigned id=i?DMA0_INTID:A733_THERMAL_INTID;
        memset(gicd,0,sizeof(gicd));gicd_write(0,0x30);gicd_write(4,10);gicd_write(0x200+4*(id/32),1u<<(id%32));
        assert(!a733_irq_table_route(HZ,a733_smp_counter)&&!gicd_read(0x88)&&!gicd_read(0x90));
    }
    printf("PASS interrupt table: %s on CPU0; %u other INTIDs and 7 other cores refused; routes programmed\n",
        ROUTES==2?"thermal INTID 71 and DMA0 INTID 143":"thermal INTID 71",988u-ROUTES);
}

/* Report periods: the board owners, the capture client, the radio owner and
 * network client, and the Bluetooth owner and headset client every tick, the
 * recognizer every 5 ms and the thermal reader every 25 ms. */
static const unsigned period_ms[OS_SERVICES]={1,1,1,1,1,5,25,
#ifdef SUNSTONE_A733_OS_AUDIO
    1,
#endif
#ifdef SUNSTONE_A733_OS_RADIO
    1,1,
#endif
#ifdef SUNSTONE_A733_OS_HEADSET
    1,1,
#endif
};
/* One supervision run in 1 ms steps. The supervisor runs every 20 ms from
 * time 0, as at scheduler start. Every service first reports at 0.3 s, except
 * service `late`, which first reports at late_at, and then at its own period
 * until stall_at, when service `stalled` stops. Records the expiry time of the
 * modeled watchdog (0 if it never expired before `end`). */
struct run { uint64_t expired,refused,reloads,max_gap,first_reload; };
static struct run supervise(int stalled,uint64_t stall_at,uint64_t end,int never,int late,uint64_t late_at)
{
    struct run r={0,0,0,0,0};
    uint64_t start=HZ*3/10,previous=0;int parked=0;
    ticks=0;expires=4*HZ;reloads=0;last_reload=0;
    for(uint64_t ms=0;ticks<end;++ms){
        ticks=ms*(HZ/1000);
        if(ticks>=expires){r.expired=ticks;break;}
        for(int s=0;s<OS_SERVICES;++s){
            if(ticks<(s==late?late_at:start)||(s==stalled&&(never||ticks>=stall_at)))continue;
            if(ms%period_ms[s]==0)board_service_progress((unsigned)s);
        }
        if(!parked&&ms%20==0){
            if(board_resident_renew(&ticks,4*HZ)!=1){parked=1;r.refused=ticks;}
            if(last_reload!=previous){
                if(!previous)r.first_reload=last_reload;
                if(previous&&last_reload-previous>r.max_gap)r.max_gap=last_reload-previous;
                previous=last_reload;
            }
        }
    }
    r.reloads=reloads;return r;
}
static void supervision_cases(void)
{
    enum { USB0=1 };
    assert(!strcmp(os_services[USB0].name,"board-usb0"));
    /* Healthy for 120 modeled seconds: renewal every second, no expiry. */
    pid_t child=fork();
    if(!child){
        struct run r=supervise(-1,0,120*HZ,0,-1,0);
        assert(!r.expired&&!r.refused&&r.reloads>=115&&r.max_gap<=OS_RENEW_US*24+HZ/50);
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    /* USB0 first reports at 1.5 s, after the supervisor has run for 1.5 s and
     * the other services for 1.2 s: no close, the first renewal follows the
     * first full set of reports, and renewal continues. */
    child=fork();
    if(!child){
        struct run r=supervise(-1,0,30*HZ,0,USB0,HZ*3/2);
        assert(!r.expired&&!r.refused&&r.first_reload>=HZ*3/2&&r.first_reload<=HZ*3/2+HZ/50&&r.reloads>=28);
        _exit(0);
    }
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    /* Each service stalling after 70 s stops renewal within its window and
     * lets the watchdog expire within four seconds of the last renewal. */
    for(int s=0;s<OS_SERVICES;++s){
        child=fork();
        if(!child){
            uint64_t at=70*HZ;
            struct run r=supervise(s,at,90*HZ,0,-1,0);
            uint64_t window=(uint64_t)os_services[s].window_us*24,period=period_ms[s]*(HZ/1000);
            /* The last report came at most one period before the stall. */
            assert(r.refused>at-period+window&&r.refused<=at+window+HZ/50);
            assert(last_reload<=r.refused&&r.expired==last_reload+4*HZ);
            assert(r.expired<=at+window+OS_RENEW_US*24+4*HZ);
            _exit(0);
        }
        assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
        printf("stalled %s: renewal stopped within its %u ms window, watchdog expired\n",
            os_services[s].name,(unsigned)(os_services[s].window_us/1000));
    }
    /* A service that never reports permits no renewal and no early close: the
     * loader's first expiry resets the board at four seconds. */
    for(int s=0;s<OS_SERVICES;++s){
        child=fork();
        if(!child){struct run r=supervise(s,0,10*HZ,1,-1,0);
            assert(!r.reloads&&!r.refused&&r.expired==4*HZ);
            /* Were the watchdog not to reset, the first pass at the expiry closes. */
            expires=UINT64_MAX;ticks=4*HZ-1;assert(board_resident_renew(&ticks,4*HZ)==1);
            ticks=4*HZ;assert(!board_resident_renew(&ticks,4*HZ)&&!reloads);
            _exit(0);}
        assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    }
    /* A reported service that stalls before another first reports closes at
     * once: thermal reports from 0.3 s and stops at 0.5 s, USB0 never reports. */
    child=fork();
    if(!child){struct run r=supervise(OS_SERVICE_THERMAL,HZ/2,10*HZ,0,USB0,UINT64_MAX);
        assert(!r.reloads&&r.refused>HZ/2-HZ/40+HZ&&r.refused<=HZ/2+HZ+HZ/50&&r.expired==4*HZ);_exit(0);}
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    /* A refused reload ends supervision. */
    child=fork();
    if(!child){reload_fails=1;struct run r=supervise(-1,0,10*HZ,0,-1,0);
        assert(!r.reloads&&r.refused&&r.expired==4*HZ);_exit(0);}
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    /* Resident board_live: deadline_us = UINT64_MAX/24 exceeds every counter/24. */
    const uint64_t deadline_us=UINT64_MAX/24;
    assert(61*HZ/24<deadline_us&&(UINT64_MAX-1)/24<deadline_us+1&&deadline_us-10000<deadline_us);
    printf("PASS resident supervision: 120 s renewed; late first report renews; each of %u stalled services ends renewal; "
        "each missing service expires at 4 s without an early close; stall before full startup closes; refused reload expires\n",
        (unsigned)OS_SERVICES);
}

#define SESSION ((struct os_session *)(uintptr_t)A733_BOARD_SESSION_ADDRESS)
static void nominal_session(struct os_session *s,uint64_t generation)
{
    memset(s,0,sizeof(*s));
    struct a733_board_session *b=&s->board;
    b->magic=A733_BOARD_SESSION_MAGIC;b->version=A733_BOARD_SESSION_VERSION;b->bytes=sizeof(*b);
    b->epoch=0x1234;b->generation=generation;b->lease=HZ;memcpy(b->image,a733_smp_build_id,32);
    memset(b->qualification,0x51,32);memset(b->recovery,0x51,32);memset(b->wiring,0x52,32);memset(b->memory,0x53,32);
    b->topology=1;b->cold_master=2;b->memory_authority=3;b->recovery_authority=4;
    b->workload=4;b->latch_identity=5;b->latch_owner=6;b->latch_known_mask=0x3fff;
    b->pmic_owner=11;b->usb0_owner=12;b->typec_owner=13;b->usb2_owner=14;
    for(unsigned i=0;i<3;++i)b->fabric_hz[i]=200000000;
    b->usb_supply_uv[0]=b->usb_supply_uv[1]=3300000;b->usb0_cold_reference=1;b->usb2_revision=1;
    b->source_min_mv=4750;b->source_max_mv=5250;b->source_ma=500;b->zero_threshold_max_mv=600;
    b->fusb_device_id=0x81;
#if defined(SUNSTONE_A733_OS_RADIO) || defined(SUNSTONE_A733_OS_HEADSET)
    /* The SID authority and, for the headset, the Bluetooth profile digest. */
    b->sid_authority=21;memset(b->sid_evidence,0x54,32);
#endif
#ifdef SUNSTONE_A733_OS_RADIO
    /* Workload 1: the board's radio station with storage. */
    b->workload=1;b->radio_profile_address=0x43000000;b->radio_profile_bytes=1872;
    memset(b->radio_profile_digest,0x55,32);
    b->latch_m_identity=22;b->latch_m_owner=23;b->latch_m_known_mask=0x3f;
#endif
    s->magic=A733_OS_SESSION_MAGIC;s->version=OS_SESSION_VERSION;s->bytes=sizeof(*s);
    s->thermal_owner=15;s->thermal_authority=16;
    memset(s->thermal_initial,1,32);memset(s->thermal_electrical,2,32);
    memset(s->thermal_clocks,3,32);memset(s->thermal_supervision,4,32);
    s->apb1=0;s->apb1_hz=24000000;s->alarm_mask=31;s->critical_mask=31;
    for(unsigned i=0;i<5;++i){s->limits[i][0]=1500;s->limits[i][1]=1600;s->limits[i][2]=1400;}
#ifdef SUNSTONE_A733_OS_AUDIO
    struct os_audio_session *a=&s->audio;
    a->magic=OS_AUDIO_SESSION_MAGIC;a->version=OS_AUDIO_SESSION_VERSION;a->bytes=sizeof(*a);
    memset(a->evidence,5,32);a->dma.owner=17;a->dma.handoff=a->dma_evidence=b->lease;
    memcpy(a->dma.fabric,b->fabric,sizeof(b->fabric));memcpy(a->dma.rate_hz,b->fabric_hz,sizeof(b->fabric_hz));
    a->dma_controller=0x04601000;a->dma_established=OS_AUDIO_DMA_ESTABLISHED;
    a->dma_owned_channels=0xffff;a->dma_service_channels=1;a->capture_samples=48000;
#endif
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    /* A grant that names no cluster: every clock and voltage stays. */
    struct a733_cpu_clock_grant_record *g=&s->cpu_clock;
    g->magic=A733_CPU_CLOCK_GRANT_MAGIC;g->version=A733_CPU_CLOCK_GRANT_VERSION;g->bytes=sizeof(*g);g->owner=18;
#endif
}
/* The contract this lifetime's producer writes: the stage-2 loader for the
 * resident image, the FEL host for the supervised one. */
static struct a733_smp_contract nominal_contract(const struct os_session *s,uint64_t armed)
{
    struct a733_smp_contract c;memset(&c,0,sizeof(c));
    c.magic=A733_SMP_CONTRACT_MAGIC;c.version=2;c.bytes=sizeof(c);c.epoch=s->board.epoch;
    c.generation=s->board.generation;c.inverse_generation=~c.generation;c.lease=s->board.lease;
    memcpy(c.build_id,s->board.image,32);c.armed_at=armed;c.startup_deadline=armed+HZ;
#ifdef SUNSTONE_BOARD_RESIDENT
    c.diagnostic_deadline=A733_SMP_NO_DEADLINE;c.expires_at=armed+4*HZ;
    c.watchdog_mode=0x41;c.watchdog_configuration=1;c.watchdog_output=2;
#else
    c.diagnostic_deadline=c.expires_at=armed+A733_BOARD_SUPERVISED_SECONDS*HZ;
#endif
    c.reserved[0]=A733_BOARD_SESSION_ADDRESS;c.reserved[1]=sizeof(*s);
    storage_digest_bytes(s,sizeof(*s),(uint8_t *)&c.reserved[2]);
    return c;
}
/* A board session without the radio fields, as workloads 0 and 4 require. */
static void without_radio(struct a733_board_session *b,unsigned workload)
{
    b->workload=workload;b->radio_profile_address=b->radio_profile_bytes=0;memset(b->radio_profile_digest,0,32);
    b->latch_m_identity=b->latch_m_owner=0;b->latch_m_known_mask=b->latch_m_value=0;
}
static void admission_cases(void)
{
    const uint64_t armed=100*HZ,now=armed+HZ/10;
    nominal_session(SESSION,9);
    struct a733_smp_contract c=nominal_contract(SESSION,armed);
    assert(a733_smp_contract_valid(&c,now));
    unsigned refused=0;
#define CONTRACT_REFUSED(change) do{nominal_session(SESSION,9);c=nominal_contract(SESSION,armed);change; \
    assert(!a733_smp_contract_valid(&c,now));++refused;}while(0)
    /* Session changes are re-sealed, so only the rule itself refuses. */
#define SESSION_REFUSED(change) do{nominal_session(SESSION,9);change;c=nominal_contract(SESSION,armed); \
    assert(!a733_smp_contract_valid(&c,now));++refused;}while(0)
    CONTRACT_REFUSED(c.magic=A733_BOARD_SUPERVISED_CONTRACT_MAGIC);
    CONTRACT_REFUSED(c.magic^=0x10000);
    CONTRACT_REFUSED(c.version=1);
    CONTRACT_REFUSED(c.reserved[1]=sizeof(struct a733_board_session));
    CONTRACT_REFUSED(c.reserved[3]^=1);
    CONTRACT_REFUSED(c.epoch^=1);
    CONTRACT_REFUSED(c.build_id[0]^=1);
    CONTRACT_REFUSED(c.startup_deadline=armed+2*HZ);
    CONTRACT_REFUSED(c.armed_at=now+1;c.startup_deadline=c.armed_at+HZ);
    CONTRACT_REFUSED(c.fel_flag=0x5aa5a55a);
    CONTRACT_REFUSED(c.redirect_flag=0xfa50392f;c.redirect_entry=0x20);
    CONTRACT_REFUSED(c.watchdog_mode=c.watchdog_mode?0:0x41);
#ifdef SUNSTONE_BOARD_RESIDENT
    CONTRACT_REFUSED(c.diagnostic_deadline=armed+60*HZ);
    CONTRACT_REFUSED(c.expires_at=armed+60*HZ);
    CONTRACT_REFUSED(c.watchdog_mode=c.watchdog_configuration=c.watchdog_output=0);
#else
    CONTRACT_REFUSED(c.diagnostic_deadline=A733_SMP_NO_DEADLINE;c.expires_at=armed+4*HZ);
#endif
    SESSION_REFUSED(SESSION->board.workload=2);
    SESSION_REFUSED(SESSION->thermal_owner=SESSION->board.pmic_owner);
    SESSION_REFUSED(SESSION->thermal_owner=SESSION->board.usb2_owner);
    SESSION_REFUSED(SESSION->magic^=1);
    SESSION_REFUSED(SESSION->bytes=592);
    SESSION_REFUSED(SESSION->alarm_mask=15);
    SESSION_REFUSED(SESSION->limits[2][2]=1501);
    SESSION_REFUSED(SESSION->apb1=0x03000003;SESSION->apb1_hz=0);
    SESSION_REFUSED(SESSION->reserved[5]=1);
    SESSION_REFUSED(SESSION->board.deadline_us=1);
#ifdef SUNSTONE_A733_OS_RADIO
    /* The radio build admits workload 1 only, and the board's radio rules apply. */
    SESSION_REFUSED(without_radio(&SESSION->board,0));
    SESSION_REFUSED(without_radio(&SESSION->board,4));
    SESSION_REFUSED(SESSION->board.radio_profile_address=0x43001000);
    SESSION_REFUSED(SESSION->board.latch_m_known_mask=0x1f);
#else
    /* Without the radio build, a board session with the radio is refused. */
    SESSION_REFUSED(SESSION->board.workload=1;SESSION->board.sid_authority=21;memset(SESSION->board.sid_evidence,0x54,32);
        SESSION->board.radio_profile_address=0x43000000;SESSION->board.radio_profile_bytes=1872;
        memset(SESSION->board.radio_profile_digest,0x55,32);SESSION->board.latch_m_identity=22;
        SESSION->board.latch_m_owner=23;SESSION->board.latch_m_known_mask=0x3f);
    (void)without_radio;
#endif
#ifdef SUNSTONE_A733_OS_HEADSET
    /* The headset build requires the SID authority and the profile digest. */
    SESSION_REFUSED(SESSION->board.sid_authority=0);
    SESSION_REFUSED(memset(SESSION->board.sid_evidence,0,32));
#endif
#ifdef SUNSTONE_A733_OS_AUDIO
    struct os_audio_session *a=&SESSION->audio;
    SESSION_REFUSED(a->magic^=1);
    SESSION_REFUSED(a->version=2);
    SESSION_REFUSED(a->bytes=sizeof(*a)-8);
    SESSION_REFUSED(memset(a->evidence,0,32));
    SESSION_REFUSED(a->dma.owner=0);
    SESSION_REFUSED(a->dma.owner=SESSION->thermal_owner);
    SESSION_REFUSED(a->dma.owner=SESSION->board.usb0_owner);
    SESSION_REFUSED(a->dma.handoff^=1);
    SESSION_REFUSED(a->dma_evidence^=1);
    SESSION_REFUSED(a->dma.deadline_us=1);
    SESSION_REFUSED(a->dma_established=0x5e);
    /* The other lifetime's handoff: supervised 0x5f against resident 0x3f. */
    SESSION_REFUSED(a->dma_established=OS_AUDIO_DMA_ESTABLISHED^0x60);
    SESSION_REFUSED(a->dma_owned_channels=0xfffe);
    SESSION_REFUSED(a->dma_service_channels=3);
    SESSION_REFUSED(a->dma_controller=0x04602000);
    SESSION_REFUSED(a->dma.rate_hz[1]^=1);
    SESSION_REFUSED(a->audio_cold=2);
    SESSION_REFUSED(a->capture_samples=OS_AUDIO_MIN_SAMPLES-1);
    SESSION_REFUSED(a->capture_samples=OS_AUDIO_MAX_SAMPLES+1);
    SESSION_REFUSED(a->reserved[6]=1);
    /* A contract for the 808-byte session cannot admit the audio image. */
    CONTRACT_REFUSED(c.reserved[1]=A733_OS_SESSION_BYTES);
#endif
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
    /* The grant record follows a733_cpu_clock_grant_load against this
     * session, and its owner is distinct from every other owner. */
    struct a733_cpu_clock_grant_record *g=&SESSION->cpu_clock;
    SESSION_REFUSED(g->magic^=1);
    SESSION_REFUSED(g->bytes=95);
    SESSION_REFUSED(g->owner=0);
    SESSION_REFUSED(g->owner=SESSION->thermal_owner);
    SESSION_REFUSED(g->owner=SESSION->board.pmic_owner);
    SESSION_REFUSED(g->changes=1);
    SESSION_REFUSED(g->clusters=4);
    SESSION_REFUSED((g->clusters=2,g->max_mhz[1]=2002,g->changes=1,memset(g->approval,7,32),g->generation=9));
    SESSION_REFUSED((g->clusters=2,g->max_mhz[1]=2000,g->changes=1,memset(g->approval,7,32),
        g->epoch=0x1234,g->generation=9));
    SESSION_REFUSED((g->clusters=2,g->max_mhz[1]=2002,g->changes=65,memset(g->approval,7,32),
        g->epoch=0x1234,g->generation=9));
    /* One cluster per approved step: a grant of both is refused. */
    SESSION_REFUSED((g->clusters=3,g->max_mhz[0]=1196,g->max_mhz[1]=2002,g->changes=2,memset(g->approval,7,32),
        g->epoch=0x1234,g->generation=9));
    /* A grant of 2,002 MHz on CPU-B for this session is admitted. */
    nominal_session(SESSION,9);g->clusters=2;g->max_mhz[1]=2002;g->changes=1;memset(g->approval,7,32);
    g->epoch=0x1234;g->generation=9;c=nominal_contract(SESSION,armed);
    assert(a733_smp_contract_valid(&c,armed+HZ/10));
    CONTRACT_REFUSED(c.reserved[1]=OS_SESSION_BASE_BYTES);
#endif
    /* A start after the startup deadline is refused. */
    nominal_session(SESSION,9);c=nominal_contract(SESSION,armed);
    assert(!a733_smp_contract_valid(&c,armed+HZ));++refused;
    printf("PASS %s%s%s%s%s OS admission: nominal contract and session accepted, %u refusals\n",
#ifdef SUNSTONE_BOARD_RESIDENT
        "resident",
#else
        "supervised",
#endif
#ifdef SUNSTONE_A733_OS_AUDIO
        " audio",
#else
        "",
#endif
#ifdef SUNSTONE_A733_OS_RADIO
        " radio",
#else
        "",
#endif
#ifdef SUNSTONE_A733_OS_HEADSET
        " headset",
#else
        "",
#endif
#ifdef SUNSTONE_A733_OS_CPU_CLOCK
        " cpu-clock",
#else
        "",
#endif
        refused);
}
static void *read_file(const char *path,size_t bytes,void *out)
{
    FILE *f=fopen(path,"rb");assert(f);
    assert(fread(out,1,bytes,f)==bytes&&fgetc(f)==EOF);fclose(f);return out;
}
int main(int argc,char **argv)
{
    assert(mmap((void *)(uintptr_t)A733_BOARD_SESSION_ADDRESS,4096,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void *)(uintptr_t)A733_BOARD_SESSION_ADDRESS);
    if(argc==4&&!strcmp(argv[1],"--write-session")){
        nominal_session(SESSION,strtoull(argv[3],0,0));
        FILE *f=fopen(argv[2],"wb");assert(f&&fwrite(SESSION,1,sizeof(*SESSION),f)==sizeof(*SESSION));fclose(f);
        return 0;
    }
    if(argc==5&&!strcmp(argv[1],"--admit")){
        struct a733_smp_contract c;read_file(argv[2],sizeof(c),&c);read_file(argv[3],sizeof(*SESSION),SESSION);
        int ok=a733_smp_contract_valid(&c,strtoull(argv[4],0,0));
        printf("%s\n",ok?"admitted":"refused");return ok?0:1;
    }
    assert(argc==1);
    irq_cases();
    supervision_cases();
    admission_cases();
    return 0;
}
