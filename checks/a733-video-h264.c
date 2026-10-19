/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reuse resource-model helpers, not the preserved JPEG runtime cases. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main preserved_jpeg_cases_not_executed
#include "a733-video-platform.c"
#undef main
#pragma clang diagnostic pop
#include "a733_video_h264.h"
struct h264_model {struct model common;unsigned variant,status_reads,copies,flush,decode,ack,header_bits;
    uint32_t sram[1024],sram_offset;};
static int admit_h264(void *v,const struct a733_video_platform_config *c,
 const struct a733_h264_job *j,uint64_t d,uint64_t *t)
{assert(j&&j->buffers[A733_H264_PICTURE_INFO].bytes==0x21000);
 return admit(v,c,NULL,d,t);}
static int memory_h264(void *v,uint64_t t,const struct a733_h264_job *j,uint64_t d)
{struct h264_model *m=v;(void)d;assert(t==42&&j&&!m->common.lock);++m->common.memory;return 1;}
static int copied_h264(void *v,uint64_t t,uint64_t d)
{
    struct h264_model *m=v;assert(t==42&&d==100000&&m->decode&&m->ack&&!m->common.lock);
    assert(m->common.ve[0x220/4]==0x400&&m->status_reads>=3);
    ++m->copies;return m->variant==5?-1:m->copies>1;
}
static int h264_resource_read(void *v,uintptr_t address,uint32_t *out)
{if(address==0x07090160){*out=0x10000002;return 1;}return read_any(v,address,out);}
static int read_h264(void *v,uintptr_t address,uint32_t *out)
{
    struct h264_model *m=v;
    if(address==0x07090160){*out=0x10000002;return 1;}
    if(address==VE+0x228&&m->decode){
        assert(m->common.lock);++m->status_reads;
        if(m->variant==3){*out=3;return 1;}
        if(m->variant==2||m->status_reads<3){*out=1|(1u<<23);
            assert(!m->copies&&!m->ack);if(m->variant==2&&m->status_reads==3)m->common.now=100000;
        }else *out=1;
        return 1;
    }
    return read_reg(v,address,out);
}
static int write_h264(void *v,uintptr_t address,unsigned width,uint32_t value)
{
    struct h264_model *m=v;
    if(address==VE+0x224&&value==7)assert(!m->decode);
    if(address==VE+0x224&&(value&255)==3){assert(!m->decode&&value>>8<=32);m->flush+=value>>8;}
    if(address==VE+0x224&&value==8){assert(m->flush==m->header_bits&&!m->decode);++m->decode;
        if(m->variant==4)m->common.now=100000;}
    if(address==VE+0x228){assert(value==1&&m->decode&&m->status_reads>=3);++m->ack;}
    if(address==VE+0x2e0)m->sram_offset=value/4;
    if(address==VE+0x2e4){assert(m->sram_offset<1024);m->sram[m->sram_offset++]=value;}
    return write_reg(v,address,width,value);
}
int main(int argc,char **argv)
{
    assert(argc==3);uint8_t bytes[A733_H264_INPUT_BYTES];FILE *f=fopen(argv[1],"rb");assert(f);
    size_t count=fread(bytes,1,sizeof(bytes),f);assert(count>0&&fgetc(f)==EOF&&!fclose(f));
    for(unsigned variant=0;variant<9;++variant){
        struct h264_model m={.variant=variant};setup(&m.common,UINT64_C(0x3331000021320));
        struct a733_h264_picture picture;assert(a733_h264_parse(bytes,count,&picture));m.header_bits=picture.header_bits;
        m.common.ve[0xb0/4]=variant==1?0x80000000:0;m.common.ve[0x228/4]=0;
        if(variant==8)m.common.ve[0x5c/4]|=0x10;
        if(variant==6)m.common.ve[0xe0/4]=0xdead;
        struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,now,&m));
        struct a733_gpu_resource_manager power={0};struct a733_gpu_resource_ops po={now,h264_resource_read,unused_update,unused_rail};
        assert(a733_gpu_resources_init(&power,&po,&m));uint64_t pt=0;struct a733_gpu_power_resources ready;
        assert(a733_gpu_resources_acquire(&power,2,9,100000,&pt,&ready)==A733_GPU_RESOURCE_OK);
        struct a733_video_platform_config c={.clocks=&clocks,.power=&power,.power_client=2,.owner=99,.handoff=123,
            .pck={pt,9,A733_GPU_RESOURCE_STABLE},.module=0x80000001,.module_hz=234000000,
            .fabric={0,0x80000000,0x80000000},.fabric_hz={24000000,24000000,24000000}};
        a733_clock_mask_t scopes;assert(a733_video_platform_scopes(&c,&scopes));
        assert(a733_clock_acquire(&clocks,99,scopes,A733_CLOCK_VE_DEC,100000,&c.clock)==A733_CLOCK_OK);
        struct a733_h264_platform_ops ops={now,admit_h264,memory_h264,copied_h264,enter,leave,read_h264,write_h264};
        struct a733_h264_platform platform={0};assert(a733_h264_platform_init(&platform,&c,&ops,&m));
        const uint64_t bases[]={0x10001000,0x10010000,0x10014000,0x10020000,0x10044000,0x10048000};
        const uint64_t caps[]={4096,4096,4096,0x21000,4096,0xc000};
        struct a733_h264_job job={.input=bytes,.input_bytes=count};
        for(unsigned i=0;i<A733_H264_BUFFERS;++i)job.buffers[i]=(struct a733_h264_span){bases[i],caps[i]};
        struct a733_h264_run run={0};
        struct a733_h264_job bad=job;bad.buffers[A733_H264_MV].iova=0x10040000;
        assert(!a733_h264_begin(&run,a733_video_platform_h264_ops(),&platform,&bad,100000));
        if(variant==7)bytes[4]^=0x80;
        assert(a733_h264_begin(&run,a733_video_platform_h264_ops(),&platform,&job,100000));
        int result=0;for(unsigned poll=0;!result&&poll<4000;++poll)result=a733_h264_poll(&run);
        assert(result==(variant?-1:1));assert(m.common.drain==0&&run.token==42);
        assert(run.state==(variant?A733_H264_FAULT:A733_H264_COPIED));
        if(variant==1||variant==2||variant==3||variant==4||variant==6||variant==7||variant==8)assert(!m.copies);
        if(variant==6||variant==7)assert(!m.common.memory&&!m.common.writes);
        assert(a733_clock_token_status(&clocks,&c.clock)==A733_CLOCK_OK);
        assert(a733_gpu_resources_status(&power,2,&c.pck)==A733_GPU_RESOURCE_OK);
        if(!variant){
            assert(m.copies==2&&m.decode==1&&m.ack==1&&m.flush==m.header_bits);
            f=fopen(argv[2],"wb");assert(f);
            assert(fwrite(m.common.ve,1,sizeof(m.common.ve),f)==sizeof(m.common.ve));
            assert(fwrite(m.sram,1,sizeof(m.sram),f)==sizeof(m.sram)&&!fclose(f));
        }
        puts((const char*[]){"PASS nominal and finish+writeback-busy pending","PASS LBC refusal",
            "PASS writeback-busy deadline retained","PASS device error retained","PASS late trigger retained",
            "PASS private copy refusal retained","PASS identity before memory","PASS forbidden NAL bit refusal",
            "PASS VCU enable refusal"}[variant]);
        if(variant==7)bytes[4]^=0x80;
    }
    return 0;
}
