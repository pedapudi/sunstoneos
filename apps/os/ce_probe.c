/* The entropy stage of the radio service (docs/status/security.md, "CE board probe"): the vendor's non-secure bring-up
 * with a reset-and-gate cycle at admission, a buffer from the unified allocator, one 32-byte TRNG task, and the record.
 * The CE belongs to the radio service under the radio grant (its only user, WPA2 needs the entropy); the stage runs in
 * the radio owner task from os_radio_scan_step when the profile's stage limit is the entropy stage. */
#include "os.h"
#include "../board/application.h"
#include "a733_ce.h"
#include "a733_ce_clock.h"
#include "a733_ce_native.h"
#include "a733_ce_probe.h"
#include "a733_mapping_check.h"
#include "a733_mmio.h"
#include "aarch64_device_memory.h"
#include "os_radio_result.h"
#include "sunstone_crumb.h"
#include "umem.h"
#include <string.h>

struct umem *os_large_umem(void);

/* The record goes out through the radio's descriptor (include/os_radio_result.h); tools/radio_result_channel.py tells it
 * from a radio record by its magic. */
extern struct os_radio_result_descriptor os_radio_result_descriptor;
static struct a733_ce_probe_record published __attribute__((aligned(64)));

#define CE_OWNER UINT64_C(0x434550524f4245) /* "CEPROBE" */
#define STEP_BOUND_US 2000000u
static struct {
    struct a733_ce_probe probe;
    struct a733_ce engine;
    struct a733_ce_clock clock;
    struct a733_ce_config config;
    struct a733_clock_token token;
    struct umem_buf buffer;
    struct a733_ce_memory *memory;
    uint64_t physical;
    unsigned started, finished, buffer_state;
    /* Why the last admission was refused, for the record: bit 0 scheduler, 1 board, 2 mapping, 3 GIC mask, bits 7:4 the
     * first unmapped address of mappings() plus one; bit 8 the clock token was refused. */
    unsigned denied;
    unsigned detail; /* The failing step's own detail for the record, set where the step fails. */
    unsigned quiet;  /* The bring-up runs for the association stage's entropy: no CE record is published. */
} ce;
/* The association stage's reservoir of CE bytes: UNQUALIFIED, for bring-up only (docs/radio-entropy.md). */
static struct { uint8_t bytes[256]; unsigned count, requested, failed; } pool;

static uint64_t deadline_us(void){return board.session.deadline_us-10000;}
static int now(void *v,uint64_t *n){return v==&ce?a733_counter_clock_now(&providers.shared.counter,n):0;}
static int enter(void *v,uint64_t d)
{return v==&ce&&board_live(NULL)?a733_platform_lock_enter(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK,d):-1;}
static void leave(void *v){if(v==&ce)a733_platform_lock_leave(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}
static void retain_lock(void *v){if(v==&ce)(void)a733_platform_lock_retain(&providers.shared.locks,A733_PLATFORM_REGISTER_LOCK);}

static int device(uintptr_t a)
{
    /* Device identity mapping; the write translation must be outer shareable. */
    static const struct a733_mapping_rule rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
    return a733_mapping_check(aarch64_device_translate,a,a,&rule);
}
/* The CE register page (non-secure bank at +0x000, whose page also holds the secure bank, which is never touched),
 * the clock controller, the RTC word and the GIC word that masks the CE interrupts. */
static unsigned unmapped(void)
{
    static const uintptr_t addresses[]={0x02002000,0x02003000,0x07090160,0x04603000,0x03400118};
    for(unsigned i=0;i<sizeof(addresses)/sizeof(addresses[0]);++i)if(!device(addresses[i]))return i+1;
    return 0;
}
static int mappings(void){return !unmapped();}
static int masked(void)
{
    if(!device(0x03400118))return 0;
    return !(a733_mmio_read32_dsb_sy_both(0x03400118)&0x300); /* INTIDs 200 and 201 */
}
/* a733_ce_request moves the controller's deadline to the request's, which is earlier than the session's: any deadline
 * up to the session's is the same authority. */
static int authorize(void *v,const struct a733_ce_config *c,const struct a733_clock_token *t,const struct a733_ce_memory *m)
{
    if(v!=&ce||!board_live(NULL)||!mappings()||!masked()||!ce.engine.initialized||m!=ce.memory||
       c->owner!=CE_OWNER||c->handoff!=board.session.generation||c->profile!=A733_CE_TRNG_ONLY||
       c->deadline_us>deadline_us()||c->health_profile||c->configure_health||
       t->id!=ce.token.id||t->owner!=ce.token.owner||t->shared!=ce.token.shared||t->exclusive!=ce.token.exclusive)return 0;
    for(unsigned i=0;i<4;++i)if(c->health[i])return 0;
    for(unsigned i=0;i<3;++i)if(c->fabric[i]!=board.session.fabric[i]||c->fabric_hz[i]!=board.session.fabric_hz[i])return 0;
    struct umem_extent e;int state;
    if(umem_describe(os_large_umem(),&ce.buffer,&e,&state)||state!=UMEM_DEVICE||e.base!=ce.physical||
       c->descriptor_physical!=e.base||c->output_physical!=e.base+256)return 0;
    /* Every 64-byte line of the descriptor object is write-back and inner shareable at its physical address. */
    static const struct a733_mapping_rule wb={A733_MAPPING_WRITE,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!a733_mapping_check_stride(aarch64_device_translate,(uintptr_t)m,e.base,sizeof(*m),64,&wb))return 0;
    return a733_clock_token_status(&providers.shared.clocks,&ce.token)==A733_CLOCK_OK;
}
static int clock_authorize(void *v)
{
    if(v!=&ce)return 0;
    unsigned denied=0,gap;
    if(xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING)denied|=1;
    if(!board_live(NULL))denied|=2;
    if((gap=unmapped())!=0)denied|=4|gap<<4;
    else if(!masked())denied|=8;
    if(denied)ce.denied=denied;
    return !denied;
}

static int op_bring_up(void *v)
{
    (void)v;
    /* The CE clock registry token: the CE exclusive, the stable parents and RC16M shared. */
    if(a733_clock_acquire(&providers.shared.clocks,CE_OWNER,
           A733_CLOCK_PERI0|A733_CLOCK_REF|A733_CLOCK_OSC|A733_CLOCK_FABRIC|A733_CLOCK_RC16M,A733_CLOCK_CE,
           deadline_us(),&ce.token)!=A733_CLOCK_OK){ce.denied|=1u<<8;ce.detail=(ce.denied&0x1ffu)<<12;return -1;}
    ce.config=(struct a733_ce_config){.owner=CE_OWNER,.handoff=board.session.generation,.deadline_us=deadline_us(),
        .profile=A733_CE_TRNG_ONLY};
    memcpy(ce.config.fabric,board.session.fabric,sizeof(ce.config.fabric));
    memcpy(ce.config.fabric_hz,board.session.fabric_hz,sizeof(ce.config.fabric_hz));
    const struct a733_ce_clock_ops clocks={clock_authorize,now,enter,leave,retain_lock,a733_ce_native_read,a733_ce_clock_native_write};
    /* Found enabled at entry, the CE is taken down by the reverse of the bring-up first (drivers/crypto/a733_ce_clock.c). */
    if(!a733_ce_clock_provision(&ce.clock,&ce.config,&clocks,&ce,&providers.shared.clocks,&ce.token))
        {ce.detail=(ce.clock.fail_line&0xfffu)|(ce.denied&0x1ffu)<<12;return -1;} /* a write-free refusal keeps the token and the lock; after a write the provision retained them */
    return 1;
}
static void clean(const void *base,size_t bytes)
{
    for(uintptr_t a=(uintptr_t)base&~(uintptr_t)63;a<(uintptr_t)base+bytes;a+=64)
        __asm__ volatile("dc civac, %0"::"r"(a):"memory");
    __asm__ volatile("dsb sy":::"memory");
}
/* One 4 KiB device-reachable page: the descriptor object (3,520 bytes) from the unified ledger with the crypto
 * device's address limit; zeroed and cleaned by the ledger, then handed to the device. */
static int op_alloc(void *v,uint64_t *descriptor,uint64_t *output)
{
    (void)v;
    struct umem *ledger=os_large_umem();
    if(!ledger){ce.detail=1;return -1;}
    if(!ce.buffer.generation){
        /* The OS ledger has no cache-maintenance hook (apps/os/large.c), so the page is raw, as every kernel user's is:
         * zeroed and cleaned here before the device sees it. */
        struct umem_request r={.bytes=4096,.align=4096,.window=4096,.device_mask=UMEM_DEV_CRYPTO,
                               .owner=UMEM_OWNER_KERNEL,.policy=UMEM_WB,.flags=UMEM_RAW};
        int status=umem_alloc(ledger,&r,&ce.buffer);
        if(status!=UMEM_OK){ce.detail=2|(unsigned)(-status)<<4;return -1;}
        volatile uint8_t *page=(volatile uint8_t *)(uintptr_t)ce.buffer.base;
        for(unsigned i=0;i<4096;++i)page[i]=0;
        clean((const void *)(uintptr_t)ce.buffer.base,4096);
    }
    if(!umem_ready(ledger,&ce.buffer)){ce.detail=3;return -1;}
    if(!ce.buffer_state){
        int status=umem_handoff(ledger,&ce.buffer,UMEM_CPU,UMEM_DEVICE,NULL,0);
        if(status!=UMEM_OK){ce.detail=4|(unsigned)(-status)<<4;return -1;}
        ce.buffer_state=1;
    }
    ce.physical=ce.buffer.base;
    ce.memory=(struct a733_ce_memory *)(uintptr_t)ce.buffer.base; /* The kernel maps DRAM one to one. */
    *descriptor=ce.buffer.base;*output=ce.buffer.base+256;
    return 1;
}
static int op_configure(void *v)
{
    (void)v;
    ce.config.descriptor_physical=ce.physical;ce.config.output_physical=ce.physical+256;
    const struct a733_ce_ops ops={.now_us=now,.authorize=authorize,.enter=enter,.leave=leave,.retain=retain_lock,
        .read=a733_ce_native_read,.write=a733_ce_native_write,.sync=a733_ce_native_sync};
    return a733_ce_init(&ce.engine,&ce.config,&ops,&ce,&providers.shared.clocks,&ce.token,ce.memory)?1:-1;
}
static int op_request(void *v,uint64_t deadline)
{
    (void)v;
    enum a733_ce_result r=a733_ce_request(&ce.engine,deadline);
    ce.detail=(ce.engine.fail_line&0xfffu)|((unsigned)r&7u)<<16;
    return r==A733_CE_PENDING?1:r==A733_CE_BUSY?0:-1;
}
static int op_poll(void *v,uint8_t block[A733_CE_PROBE_BYTES])
{
    (void)v;
    enum a733_ce_result r=a733_ce_poll(&ce.engine,block);
    /* For the record if this poll is the last: the controller's first failing source line (bits 11:0), its stage (14:12),
     * its fault flag (15), this result (18:16), the status word's task bits (20:19) and the error word's low bits (23:21). */
    ce.detail=(ce.engine.fail_line&0xfffu)|((unsigned)ce.engine.stage&7u)<<12|(ce.engine.fault?1u<<15:0)|
        ((unsigned)r&7u)<<16|((ce.engine.status_ns>>4)&3u)<<19|(ce.engine.error_ns&7u)<<21;
    return r==A733_CE_RANDOM?1:(r==A733_CE_PENDING||r==A733_CE_BUSY)?0:-1;
}
/* A failure inside the controller retains its own resources. Before the engine exists the bring-up is complete and consistent (or retained by the clock code itself), no task has started, and nothing is retained, so the session returns with its record. */
static void op_retain(void *v){(void)v;if(ce.token.id&&ce.engine.initialized)(void)a733_clock_retain(&providers.shared.clocks,&ce.token);}
/* The failing step's own detail for the record. Bring-up: the clock provision's first failing source line (bits 11:0)
 * and the admission denial (bits 20:12). Allocation: the step that failed (bits 3:0: 1 no ledger, 2 alloc, 3 prepare,
 * 4 handoff) and the negative umem status (bits 11:4). Request and poll: see op_poll. */
static unsigned op_detail(void *v){(void)v;return ce.detail;}

static void publish(void)
{
    if(ce.finished)return;
    ce.finished=1;
    if(ce.quiet)return;
    published=ce.probe.record;
    clean(&published,sizeof(published));
    os_radio_result_descriptor.address=(uint64_t)(uintptr_t)&published;
    os_radio_result_descriptor.bytes=sizeof(published);
    os_radio_result_descriptor.magic=OS_RADIO_RESULT_MAGIC;
    clean(&os_radio_result_descriptor,sizeof(os_radio_result_descriptor));
}

/* Entry points for the grant wiring. start: once, in the owner task, before the first step. step: one bounded step per
 * pass of the owner task. done: 1 once the record is published (success or failure). */
void os_ce_probe_start(void)
{
    if(ce.started)return;
    ce.started=1;
    const struct a733_ce_probe_ops ops={&ce,now,a733_ce_native_read,op_bring_up,op_alloc,op_configure,op_request,op_poll,op_retain,op_detail};
    a733_ce_probe_init(&ce.probe,&ops);
}
void os_ce_probe_step(void)
{
    if(!ce.started||ce.finished)return;
    int r=a733_ce_probe_step(&ce.probe);
    if(r<0){
        /* The record names the failing stage; so does the crumb, like the radio's. */
        board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(BOARD)|SUNSTONE_CRUMB_FAIL_RETURN,
            ce.probe.record.failure_stage,__builtin_return_address(0));
    }
    if(r)publish();
}
int os_ce_probe_done(void){return ce.finished;}
int os_ce_probe_failed(void){return ce.finished&&ce.probe.record.failed;}

/* The unqualified entropy source of the association stage. begin: once, in the owner task. step: one bounded step per
 * pass (the bring-up and the one 32-byte task of the probe, then refills of the reservoir). ready: the reservoir holds at
 * least 128 bytes. bytes: the supplicant's entropy callback, all-or-nothing, in the owner task only; consumed bytes are
 * zeroed. failed: the engine or a task failed; nothing more is drawn. Health tests are at hardware defaults and nothing
 * here qualifies the source. */
void os_ce_entropy_begin(void){ce.quiet=1;os_ce_probe_start();}
void os_ce_entropy_step(void)
{
    if(!ce.started||pool.failed)return;
    if(!ce.finished){os_ce_probe_step();if(ce.finished&&ce.probe.record.failed)pool.failed=1;return;}
    if(ce.probe.record.failed)return;
    uint64_t t;
    if(now(&ce,&t)!=1)return;
    if(!pool.requested&&pool.count+A733_CE_PROBE_BYTES<=sizeof(pool.bytes)){
        enum a733_ce_result r=a733_ce_request(&ce.engine,t+A733_CE_PROBE_STEP_LIMIT_US);
        if(r==A733_CE_PENDING)pool.requested=1;else if(r!=A733_CE_BUSY)pool.failed=1;
    }else if(pool.requested){
        uint8_t block[A733_CE_PROBE_BYTES];
        enum a733_ce_result r=a733_ce_poll(&ce.engine,block);
        if(r==A733_CE_RANDOM){
            for(unsigned i=0;i<sizeof(block);++i){pool.bytes[pool.count+i]=block[i];((volatile uint8_t *)block)[i]=0;}
            pool.count+=A733_CE_PROBE_BYTES;pool.requested=0;
        }else if(r!=A733_CE_PENDING&&r!=A733_CE_BUSY)pool.failed=1;
    }
}
int os_ce_entropy_ready(void){return ce.finished&&!ce.probe.record.failed&&!pool.failed&&pool.count>=128;}
int os_ce_entropy_failed(void){return pool.failed||(ce.finished&&ce.probe.record.failed);}
int os_ce_entropy_bytes(void *context,uint8_t *out,size_t n)
{
    (void)context;
    if(!out||!n||n>pool.count||pool.failed)return 0;
    for(size_t i=0;i<n;++i)out[i]=pool.bytes[i];
    for(size_t i=0;i+n<pool.count;++i)pool.bytes[i]=pool.bytes[i+n];
    for(size_t i=pool.count-n;i<pool.count;++i)((volatile uint8_t *)pool.bytes)[i]=0;
    pool.count-=(unsigned)n;
    return 1;
}
/* Zero the reservoir and the blocks in flight, and clean them to DRAM: the bytes seeded the handshake's nonces. EL1 only. */
void os_ce_entropy_wipe(void)
{
    volatile uint8_t *b=pool.bytes;
    for(unsigned i=0;i<sizeof(pool.bytes);++i)b[i]=0;
    pool.count=0;pool.requested=0;pool.failed=1;
    clean(pool.bytes,sizeof(pool.bytes));
}
