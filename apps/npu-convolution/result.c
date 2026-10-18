#include "a733_npu_application.h"
#include "boot.h"
#include "result.h"
_Static_assert(A733_SMP_CONTRACT_MAGIC==UINT64_C(0x415337334f424331),"compact launcher contract");
_Static_assert(sizeof(struct a733_smp_contract)==224,"compact contract extent");
_Static_assert(sizeof(struct a733_npu_application_profile)==112,"host profile ABI");

int a733_npu_application_observer_admit(void)
{
    const struct a733_npu_application_profile *p=&a733_npu_application_boot.profile;
    const struct a733_observe_identity *id=&a733_observe_identity;
    uint64_t now=a733_smp_counter();
    return id->epoch && id->generation && id->lease && id->armed_at<id->deadline &&
        now>=id->armed_at && now<id->deadline && p->epoch==id->epoch &&
        p->generation==id->generation && p->lease==id->lease && p->npu_evidence &&
        p->command_admission && p->usb_evidence && p->pmic_evidence && p->pin_evidence &&
        p->npu_established==A733_NPU_HANDOFF_REQUIRED &&
        p->usb_established==A733_NPU_APPLICATION_USB_REQUIRED;
}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p)
{uint64_t v=0;for(unsigned i=0;i<8;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
static void put64(uint8_t *p,uint64_t value)
{for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(value>>(8*i));}
int a733_npu_application_request(const uint8_t p[64],const struct a733_observe_identity *id,
    uint32_t *sequence,uint8_t challenge[32])
{
    if(le32(p)!=A733_OBSERVE_MESSAGE_MAGIC || !le32(p+4) ||
       le32(p+8)!=A733_NPU_APPLICATION_COMMAND || le32(p+12)!=48 ||
       le64(p+16)!=id->epoch || le64(p+24)!=id->generation)return 0;
    unsigned nonzero=0;for(unsigned i=0;i<32;++i)nonzero|=p[32+i];if(!nonzero)return 0;
    *sequence=le32(p+4);for(unsigned i=0;i<32;++i)challenge[i]=p[32+i];return 1;
}
int a733_npu_application_frame(uint8_t out[512],const struct a733_observe_identity *id,
    const uint8_t challenge[32],const uint8_t build[32])
{
    if(id!=&a733_observe_identity || !a733_npu_application_observer_admit())return 0;
    const struct a733_npu_application_profile *p=&a733_npu_application_boot.profile;
    unsigned healthy=a733_npu_application_health();
    unsigned failed=atomic_load_explicit(&a733_npu_application_failed,memory_order_acquire);
    if(!healthy && !failed)return 0;
    unsigned done=atomic_load_explicit(&a733_npu_application_result.complete,memory_order_acquire);
    if(done>2)return 0;
    uint64_t start=0,end=0,job=0;
    if(done) {
        start=a733_npu_application_result.start;end=a733_npu_application_result.end;
        job=a733_npu_application_result.job;
        if(!job || start<id->armed_at || end<start || end>=id->deadline)return 0;
    }
    for(unsigned i=0;i<512;++i)out[i]=0;
    put64(out,A733_NPU_APPLICATION_FRAME);put64(out+8,UINT64_C(512)<<32|1);
    put64(out+24,id->epoch);put64(out+32,id->generation);put64(out+40,id->lease);
    put64(out+48,start);put64(out+56,end);put64(out+64,id->deadline);put64(out+72,job);
    for(unsigned i=0;i<32;++i){out[80+i]=challenge[i];out[112+i]=build[i];}
    put64(out+144,0x03600000);put64(out+152,p->npu_evidence);
    put64(out+160,p->command_admission);put64(out+168,p->usb_evidence);put64(out+176,id->armed_at);
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<32;++j)
        out[184+i*32+j]=a733_npu_application_artifact_hashes[i][j];
    if(done) {
        unsigned mismatch=0;
        for(unsigned i=0;i<NPU_CONVOLUTION_OUTPUT_BYTES;++i) {
            out[280+i]=a733_npu_application_result.output[i];
            mismatch|=out[280+i]^a733_npu_application_reference[i];
        }
        if((done==1 && mismatch) || (done==2 && !mismatch))return 0;
        a733_observe_digest(out+280,NPU_CONVOLUTION_OUTPUT_BYTES,out+352);
        put64(out+384,NPU_CONVOLUTION_OUTPUT_BYTES);
    }
    (void)a733_npu_application_health();
    unsigned latest=atomic_load_explicit(&a733_npu_application_failed,memory_order_acquire);
    if(latest)failed=latest;
    put64(out+16,failed ? 3 : done==1 ? 0 : done==2 ? 2 : 1);
    put64(out+392,failed);
    a733_observe_digest(out,480,out+480);return 1;
}
