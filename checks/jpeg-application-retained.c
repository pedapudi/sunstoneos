/* Reuse existing model helpers; execute only the new retained-copy contract. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main preserved_platform_cases_not_executed
#include "a733-video-platform.c"
#undef main
#pragma clang diagnostic pop
static unsigned copies;
static int reject;
static int private_copy(void *v,uint64_t token,uint64_t deadline)
{
    struct a733_video_platform *p=v;struct model *m=p->cookie;
    assert(m->trigger&&m->drain==0&&token==42&&deadline==100000);
    ++copies;return reject?-1:copies>1;
}
static int gated_copy(void *v,uint64_t token,uint64_t deadline)
{return a733_video_platform_retained_copy(v,token,deadline,private_copy,v);}
int main(int argc,char **argv)
{
    assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);uint8_t bytes[65536]={0};
    size_t n=fread(bytes,1,sizeof(bytes),f);assert(n==155);fclose(f);
    for(reject=0;reject<2;++reject){
        copies=0;struct model m={0};setup(&m,UINT64_C(0x3331000021320));
        struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,now,&m));
        struct a733_gpu_resource_manager power={0};struct a733_gpu_resource_ops po={now,read_any,unused_update,unused_rail};
        assert(a733_gpu_resources_init(&power,&po,&m));uint64_t pt=0;struct a733_gpu_power_resources ready;
        assert(a733_gpu_resources_acquire(&power,2,9,100000,&pt,&ready)==A733_GPU_RESOURCE_OK);
        struct a733_video_platform_config c={.clocks=&clocks,.power=&power,.power_client=2,.owner=99,.handoff=123,
            .pck={pt,9,A733_GPU_RESOURCE_STABLE},.module=0x80000001,.module_hz=234000000,
            .fabric={0,0x80000000,0x80000000},.fabric_hz={24000000,24000000,24000000}};
        a733_clock_mask_t scopes;assert(a733_video_platform_scopes(&c,&scopes));
        assert(a733_clock_acquire(&clocks,99,scopes,A733_CLOCK_VE_DEC,100000,&c.clock)==A733_CLOCK_OK);
        struct a733_video_platform_ops ops={now,admit,memory,drain,enter,leave,read_reg,write_reg};
        struct a733_video_platform p={0};assert(a733_video_platform_init(&p,&c,&ops,&m));
        struct a733_video_jpeg_job job={bytes,(uint32_t)n,0x81000000,65536,0x82000000,2048,0x83000000,1024};
        struct a733_video_jpeg_run run={0};
        assert(a733_video_jpeg_begin_retained(&run,a733_video_platform_jpeg_ops(),&p,&job,100000,gated_copy));
        int r=0;for(unsigned i=0;!r&&i<2000;++i)r=a733_video_jpeg_poll(&run);
        assert(r==(reject?-1:1)&&m.drain==0&&copies==(reject?1u:2u));
        assert(run.state==(reject?A733_VIDEO_JPEG_FAULT:A733_VIDEO_JPEG_COPIED));
        unsigned before=copies;assert(a733_video_jpeg_poll(&run)==r&&copies==before);
        assert(a733_clock_token_status(&clocks,&c.clock)==A733_CLOCK_OK);
        assert(a733_gpu_resources_status(&power,2,&c.pck)==A733_GPU_RESOURCE_OK);
    }
    puts("JPEG retained copy: pending verification, distinct COPIED, rejection and permanent tokens passed");
    return 0;
}
