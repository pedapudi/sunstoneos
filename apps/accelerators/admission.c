#include "session.h"
#include "boot.h"
#include "cached.h"
#include "a733_storage_digest.h"
#include <string.h>
#if !defined(SUNSTONE_ACCELERATORS) || !defined(SUNSTONE_NPU_NN_POOL_GRAPH)
#error "Combined admission requires the complete fixed NPU graph"
#endif
#define RETAINED __attribute__((section(".npu_runtime_retained")))
static struct accelerator_session session RETAINED;
static unsigned copied RETAINED,authenticated RETAINED;
static uint8_t authenticated_digest[32] RETAINED;
const uint64_t accelerator_session_layout[8]={
    sizeof(struct accelerator_session),offsetof(struct accelerator_session,npu),
    sizeof(struct npu_session),offsetof(struct accelerator_session,gpu_initial),
    offsetof(struct accelerator_session,package_digest),
    offsetof(struct accelerator_session,package),sizeof(struct pvr_compute_package),20};
static int present(const uint8_t p[32])
{unsigned v=0;for(unsigned i=0;i<32;++i)v|=p[i];return v!=0;}
int accelerator_session_valid(const struct accelerator_session *s)
{
    if(!s||s->magic!=ACCELERATOR_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||
       !npu_session_valid(&s->npu)||!present(s->gpu_initial)||!present(s->gpu_memory)||
       !present(s->gpu_electrical)||!present(s->gpu_supervision)||
       !pvr_compute_package_valid(&s->package))return 0;
    const struct a733_npu_application_profile *p=&s->npu.boot.profile;
    const uint64_t owners[]={p->npu_owner,p->pmic_owner,p->usb_owner,p->bridge_owner};
    for(unsigned i=0;i<4;++i)
        if(owners[i]==ACCELERATOR_GPU_FIRMWARE_OWNER||
           owners[i]==ACCELERATOR_GPU_COMPUTE_OWNER)return 0;
    uint8_t digest[32];storage_digest_bytes(&s->package,sizeof(s->package),digest);
    return !memcmp(digest,s->package_digest,32);
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct accelerator_session *s=(const void *)NPU_SESSION_ADDRESS;
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=12||!c||c->magic!=ACCELERATOR_CONTRACT_MAGIC||c->version!=1||
       c->bytes!=sizeof(*c)||!c->epoch||!c->generation||!c->lease||
       c->inverse_generation!=~c->generation||memcmp(c->build_id,a733_smp_build_id,32)||
       !c->reserved[0]||c->reserved[1]!=sizeof(*s))return 0;
    uint8_t digest[32];storage_digest_bytes(s,sizeof(*s),digest);
    if(memcmp(digest,&c->reserved[2],32)||!accelerator_session_valid(s)||
       memcmp(s->npu.image,a733_smp_build_id,32)||c->epoch!=s->npu.epoch||
       c->generation!=s->npu.generation||c->lease!=s->npu.lease||
       c->reserved[0]!=s->npu.supervisor||c->armed_at>now||
       c->armed_at>UINT64_MAX-NPU_WINDOW_TICKS||
       c->startup_deadline!=c->armed_at+A733_SMP_COUNTER_HZ||
       now>=c->startup_deadline||c->diagnostic_deadline!=c->armed_at+NPU_WINDOW_TICKS||
       c->expires_at!=c->diagnostic_deadline||c->watchdog_mode||
       c->watchdog_configuration||c->watchdog_output||c->fel_flag||
       c->redirect_flag||c->redirect_entry||c->alternate_flag)return 0;
    memcpy(authenticated_digest,digest,32);authenticated=1;return 1;
}
int npu_single_core_seal(const struct a733_smp_contract *c)
{
    uint64_t el;__asm__ volatile("mrs %0,CurrentEL":"=r"(el));
    if(el!=12||!c||copied||!authenticated)return 0;
    memcpy(&session,(const void *)NPU_SESSION_ADDRESS,sizeof(session));
    uint8_t digest[32];storage_digest_bytes(&session,sizeof(session),digest);
    if(memcmp(digest,authenticated_digest,32)||!accelerator_session_valid(&session)||
       memcmp(session.npu.image,a733_smp_build_id,32)||session.npu.epoch!=c->epoch||
       session.npu.generation!=c->generation||session.npu.lease!=c->lease)return 0;
    copied=1;__asm__ volatile("dsb sy":::"memory");return 1;
}
int npu_session_copy(void)
{return copied&&a733_smp_device_preparation_admitted();}
const struct npu_session *npu_session_get(void)
{return copied?&session.npu:NULL;}
const struct accelerator_session *accelerator_session_get(void)
{return copied?&session:NULL;}
const struct pvr_compute_package *accelerator_gpu_package(void)
{return copied?&session.package:NULL;}
int a733_smp_power_address(unsigned core,uintptr_t address,int write)
{
    if(core<1||core>=8)return 0;
    uintptr_t cfg=UINT32_C(0x08001000)+(uintptr_t)core*0x1000;
    if(address==cfg||address==cfg+4||address==cfg+8||
       address==UINT32_C(0x07050200)+core*4||address==UINT32_C(0x07050220)+core*4)return 1;
    return !write&&address==UINT32_C(0x07052008)+(uintptr_t)core*0x1000;
}
