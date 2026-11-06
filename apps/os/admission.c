/* OS contract and session admission over the board's own checks. The board
 * validator runs unchanged on the board prefix; this unit adds the OS
 * magic numbers, the extension and a digest over the complete session. */
#include "os.h"
#include "a733_admission_refusal.h"
/* The OS image admits the resident lifetime as well as the supervised one; the board check below applies the resident
 * timing terms to the resident board magic. */
#define A733_BOARD_ADMITS_RESIDENT 1
#define a733_smp_contract_valid os_board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
/* An image built with a service whose retirement is qualified only for the returning lifetime (the RAM-copy DMA
 * service, the scanning radio station, the GPU qualification builds) admits that lifetime alone
 * (SUNSTONE_A733_OS_RETURN_ONLY, cmake/sunstone-os-core.cmake). An image with the USB class services, whose host stack
 * has no returning retirement, refuses the returning lifetime. Every other image admits all three. */
#ifdef SUNSTONE_A733_OS_RETURN_ONLY
#define OS_RETURN_ONLY 1
#else
#define OS_RETURN_ONLY 0
#endif
#ifdef SUNSTONE_A733_OS_USB_CLASSES
#define OS_RETURNS 0
#else
#define OS_RETURNS 1
#endif
/* The OS contract magics this image admits, by lifetime (supervised, resident, returning; 0 for a lifetime it does not
 * admit). Offline card preparation and the runner's launcher read these compiled constants, so a manifest cannot
 * relabel an image's lifetime. */
__attribute__((used,retain)) const uint64_t os_contract_profile[3]={OS_RETURN_ONLY?0:A733_OS_CONTRACT_MAGIC,
    OS_RETURN_ONLY?0:A733_OS_RESIDENT_CONTRACT_MAGIC,OS_RETURNS?A733_OS_RETURN_CONTRACT_MAGIC:0};
/* The lifetime admission verified from the contract magic (enum a733_lifetime), 0 before. EL3 monitor memory: the
 * admission below writes it once, when the contract passes, and the EL3 boot reads it for the watchdog readback, the
 * result record and the cores' startup. */
unsigned a733_smp_lifetime __attribute__((section(".bss.a733_smp_monitor")));
/* The lifetime that the contract under admission names, while its session is decoded (os_lifetime,
 * apps/os/lifetime.c); 0 outside a733_smp_contract_refusal. EL3 monitor memory. */
unsigned os_admitting_lifetime __attribute__((section(".bss.a733_smp_monitor")));
/* The lifetime that an OS contract magic names, among those this image admits; 0 for any other magic. */
static unsigned os_contract_lifetime(uint64_t magic)
{
    if(!OS_RETURN_ONLY&&magic==A733_OS_CONTRACT_MAGIC)return A733_LIFETIME_SUPERVISED;
    if(!OS_RETURN_ONLY&&magic==A733_OS_RESIDENT_CONTRACT_MAGIC)return A733_LIFETIME_RESIDENT;
    if(OS_RETURNS&&magic==A733_OS_RETURN_CONTRACT_MAGIC)return A733_LIFETIME_RETURN;
    return 0;
}
/* The EL3 result record's magic for the admitted lifetime. */
uint64_t a733_smp_result_magic(void)
{
    return a733_smp_lifetime==A733_LIFETIME_RESIDENT?A733_OS_RESIDENT_RESULT_MAGIC:
        a733_smp_lifetime==A733_LIFETIME_RETURN?A733_OS_RETURN_RESULT_MAGIC:A733_OS_RESULT_MAGIC;
}
int os_session_valid(const struct os_session *s)
{
    if(!s||!a733_board_session_valid(&s->board,0)||!os_workload_valid(s->board.workload)||
       s->magic!=A733_OS_SESSION_MAGIC||s->version!=OS_SESSION_VERSION||s->bytes!=os_session_wire_bytes(s->granted)||
       !s->thermal_owner||!s->thermal_authority||!present(s->thermal_initial,32)||
       !present(s->thermal_electrical,32)||!present(s->thermal_clocks,32)||
       !present(s->thermal_supervision,32)||s->configure_adc_clock>1||s->alarm_mask!=31||
       s->critical_mask!=31||s->reserved0||present(s->reserved,2)||s->cpu_critical_millicelsius>110000||(s->apb1&~UINT32_C(0x0300001f)))
        return 0;
    /* The thermal owner is distinct from every board owner. */
    uint64_t owners[]={s->board.pmic_owner,s->board.usb0_owner,s->board.typec_owner,s->board.usb2_owner};
    for(unsigned i=0;i<4;++i)if(owners[i]==s->thermal_owner)return 0;
    unsigned parent=s->apb1>>24,div=(s->apb1&31)+1;
    uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
    if(!hz||hz%div||s->apb1_hz!=hz/div)return 0;
    /* As in the Bluetooth image, a headset session carries the SID authority and the SHA-256 of the headset profile
     * at OS_HEADSET_PROFILE_ADDRESS (apps/os/identity.c). */
    if((s->granted&OS_GRANT_HEADSET)&&(!s->board.sid_authority||!present(s->board.sid_evidence,32)))return 0;
    for(unsigned i=0;i<5;++i)if(!s->limits[i][0]||s->limits[i][0]>=s->limits[i][1]||s->limits[i][1]>4095||
       !s->limits[i][2]||s->limits[i][2]>s->limits[i][0])return 0;
    /* Each service's record is validated only when the session grants it; an absent record grants nothing. */
    uint64_t cpu_owner=0;(void)cpu_owner;
    /* A returning session carries the CPU clock grant that its return path needs, for its board workload (2, or 3 in
     * the scanning image), both clusters, at least four changes and no performance mode. */
    const int returning=os_lifetime()==A733_LIFETIME_RETURN;
    if(returning&&!(s->granted&OS_GRANT_CPU_CLOCK))return 0;
    if(s->granted&OS_GRANT_AUDIO){
        /* The I2S image's DMA0 handoff rules, bound to this session:
         * one service channel, the board fabric, the board lease as handoff and
         * evidence identity, and a deadline that only the image derives. */
        const struct os_audio_session *a=&s->audio;
        const struct a733_dma_platform_config *d=&a->dma;
        if(a->magic!=OS_AUDIO_SESSION_MAGIC||a->version!=OS_AUDIO_SESSION_VERSION||a->bytes!=sizeof(*a)||
           !present(a->evidence,32)||!d->owner||d->owner==s->thermal_owner||d->handoff!=s->board.lease||
           a->dma_evidence!=s->board.lease||d->deadline_us||a->dma_established!=OS_AUDIO_DMA_ESTABLISHED(os_lifetime())||
           a->dma_owned_channels!=0xffff||a->dma_service_channels!=1||a->dma_controller!=0x04601000||
           memcmp(d->fabric,s->board.fabric,sizeof(d->fabric))||memcmp(d->rate_hz,s->board.fabric_hz,sizeof(d->rate_hz))||
           a->audio_cold>1||!os_audio_client_admit(a->capture_samples)||present(a->reserved,7))
            return 0;
        for(unsigned i=0;i<4;++i)if(owners[i]==d->owner)return 0;
    }
    if(s->granted&OS_GRANT_CPU_CLOCK){
        /* The grant names this session; its owner holds only the CPU clock and rail leases. The performance service
         * switches both clusters, or none, and a grant without clusters carries no perf (a733_cpu_clock_grant_load). */
        struct a733_cpu_clock_config cpu;
        if(!a733_cpu_clock_grant_load(&s->cpu_clock,s->board.epoch,s->board.generation,&cpu)||
           cpu.owner==s->thermal_owner)return 0;
#ifdef SUNSTONE_A733_OS_RADIO_SCAN
        if(returning&&(s->board.workload!=3||cpu.grant.clusters!=3||cpu.grant.changes<4||s->cpu_clock.perf))return 0;
#else
        if(returning&&(s->board.workload!=2||cpu.grant.clusters!=3||cpu.grant.changes<4||s->cpu_clock.perf))return 0;
#endif
        for(unsigned i=0;i<4;++i)if(owners[i]==cpu.owner)return 0;
        cpu_owner=cpu.owner;
        if((s->granted&OS_GRANT_NPU)&&cpu.owner==s->npu.owner)return 0;
        if((s->granted&OS_GRANT_AUDIO)&&cpu.owner==s->audio.dma.owner)return 0;
    }
    if(s->granted&OS_GRANT_NPU){
        const struct os_npu_session *n=&s->npu;
        if(n->magic!=OS_NPU_SESSION_MAGIC||n->version!=OS_NPU_SESSION_VERSION||n->bytes!=sizeof(*n)||!n->owner||
           !n->command_admission||!n->supervisor||n->pck_client>=16||n->pck_client==s->board.pck_client||
           !os_npu_session_flags_admitted(n,returning)||n->graph_region>SS_REGIONS||os_npu_declaration_admitted(n)!=NPU_DECLARATION_OK||
           !present(n->admission,32)||
           !present(n->clock,32)||!present(n->power,32)||!present(n->supervision,32)||!present(n->ddr,32)||
           n->owner==s->thermal_owner||n->owner==s->board.latch_owner||n->owner==s->board.latch_m_owner)
            return 0;
        for(unsigned i=0;i<4;++i)if(n->owner==owners[i])return 0;
#ifdef SUNSTONE_A733_OS_GPU
        if(n->pck_client==OS_GPU_CLIENT||n->owner==OS_GPU_FIRMWARE_OWNER||n->owner==OS_GPU_SESSION_OWNER)return 0;
#endif
    }
#ifdef SUNSTONE_A733_OS_DMA
    if(s->granted&OS_GRANT_DMA){
        const struct os_dma_session *dm=&s->dma;
        const struct a733_dma_handoff *h=&dm->handoff;
        const struct a733_dma_platform_config *d=&h->allowed;
        if(dm->magic!=OS_DMA_SESSION_MAGIC||dm->version!=1||dm->bytes!=sizeof(*dm)||
           !present(dm->evidence,32)||h->controller!=0x04601000||h->established!=A733_DMA_HANDOFF_REQUIRED||
           h->evidence!=s->board.lease||h->owned_channels!=0xffff||h->service_channels!=1||
           d->handoff!=s->board.lease||!d->owner||d->deadline_us||d->cold||d->route_change_mask||
           d->expected_cpu!=d->desired_cpu||d->expected_mcu!=d->desired_mcu||!(d->desired_cpu&1)||
           memcmp(d->fabric,s->board.fabric,sizeof(d->fabric))||
           memcmp(d->rate_hz,s->board.fabric_hz,sizeof(d->rate_hz))||d->owner==s->thermal_owner)return 0;
        for(unsigned i=0;i<4;++i)if(owners[i]==d->owner)return 0;
        if(cpu_owner&&d->owner==cpu_owner)return 0;
    }
#endif
    if(os_composition_refused(s->granted,os_lifetime()))return 0;
    return 1;
}
/* Zero when the contract and the session at A733_BOARD_SESSION_ADDRESS are admitted; otherwise the named refusal of
 * include/a733_admission_refusal.h. The terms are checked in the order the stop detail lists them: contract
 * identity, the session decode (which includes the manifest's fit in the lifetime), the session digest, then the
 * board prefix against the contract (build ID, timing, fields). */
unsigned a733_smp_contract_refusal(const struct a733_smp_contract *c,uint64_t now)
{
    const void *raw=(const void *)A733_BOARD_SESSION_ADDRESS;
    struct os_session decoded;const struct os_session *s=&decoded;
    if(!c)return A733_REFUSAL(A733_REFUSAL_CONTRACT_FIELD,0);
    const unsigned lifetime=os_contract_lifetime(c->magic);
    if(!lifetime)return A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION,A733_MAGIC_MAGIC);
    if(c->reserved[0]!=A733_BOARD_SESSION_ADDRESS)return A733_REFUSAL(A733_REFUSAL_SESSION_BIND,A733_BIND_CONTRACT);
    /* The decode applies the terms of the lifetime the contract names (the manifest's fit, the DMA handoff, the
     * refused compositions). */
    unsigned why=0;
    *(volatile unsigned *)&os_admitting_lifetime=lifetime;
    const int decoded_ok=os_session_decode_why(raw,c->reserved[1],&decoded,&why);
    *(volatile unsigned *)&os_admitting_lifetime=0;
    if(!decoded_ok)return A733_REFUSAL(A733_REFUSAL_SESSION_DECODE,why);
    uint8_t hash[32];storage_digest_bytes(raw,c->reserved[1],hash);
    if(memcmp(hash,&c->reserved[2],32))return A733_REFUSAL(A733_REFUSAL_SESSION_BIND,A733_BIND_DIGEST);
    /* The board check binds every remaining field against the board prefix, under the board magic of the same
     * lifetime, which selects its timing and watchdog terms. */
    struct a733_smp_contract base=*c;base.reserved[1]=sizeof(s->board);
    base.magic=lifetime==A733_LIFETIME_RESIDENT?A733_BOARD_RESIDENT_CONTRACT_MAGIC:A733_BOARD_CONTRACT_MAGIC;
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_refusal(&base,&s->board,a733_smp_build_id,now);
}
/* What the last admission check decided, for the stop that follows it (a733_smp_refusal_detail). The monitor
 * section is EL3 memory: only the EL3 admission and its stop touch it. */
static struct {uint64_t magic,generation;unsigned state,why;} refusal __attribute__((section(".bss.a733_smp_monitor")));
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    unsigned why=a733_smp_contract_refusal(c,now);
    /* The lifetime is written once. A later contract of another lifetime is refused, so no caller can change the
     * watchdog expectation after the first admission. */
    if(!why&&a733_smp_lifetime&&a733_smp_lifetime!=os_contract_lifetime(c->magic))
        why=A733_REFUSAL(A733_REFUSAL_MAGIC_VERSION,A733_MAGIC_MAGIC);
    refusal.state=why?2:1;refusal.why=why;
    if(c){refusal.magic=c->magic;refusal.generation=c->generation;}
    if(!why)a733_smp_lifetime=os_contract_lifetime(c->magic);
    return !why;
}
/* The stop of a refused admission is a733_smp_stop(1,1). Name it: the refused term of the contract check, a
 * boot state that kept the check from running, or a staged contract that changed or a result that already
 * exists after the check passed. Any other stop keeps its own detail. */
uint64_t a733_smp_refusal_detail(unsigned status,uint64_t detail)
{
    if(status!=1||detail!=1)return detail;
    if(refusal.state==2)return refusal.why;
    if(refusal.state==0)return A733_REFUSAL(A733_REFUSAL_BOOT_STATE,0);
    const volatile struct a733_smp_contract *source=(const void *)A733_SMP_CONTRACT_ADDRESS;
    if(source->magic!=refusal.magic||source->generation!=refusal.generation)
        return A733_REFUSAL(A733_REFUSAL_STALE_RESULT,A733_STALE_SOURCE);
    return A733_REFUSAL(A733_REFUSAL_STALE_RESULT,A733_STALE_RESULT);
}
