/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define main unused_jpeg_cases
#include "a733-video-platform.c"
#undef main
#pragma clang diagnostic pop
#include "a733_video_h264.h"
struct sequence_model {
    struct model common;
    struct a733_h264_run *run;
    unsigned variant,decoding,reads,flush,triggers,completed,copies[2];
    uint32_t sram[1024],sram_offset,registers[2][2048];
};
static int resource_read(void *v,uintptr_t a,uint32_t *out)
{if(a==0x07090160){*out=0x10000002;return 1;}return read_any(v,a,out);}
static int sequence_admit(void *v,const struct a733_video_platform_config *c,const struct a733_h264_job *j,uint64_t d,uint64_t *token)
{assert(j->buffers[A733_H264_PICTURE_INFO].bytes==0x21000);return admit(v,c,NULL,d,token);}
static int sequence_memory(void *v,uint64_t token,const struct a733_h264_job *j,uint64_t d)
{struct sequence_model *m=v;(void)d;assert(token==42&&j&&!m->common.lock);++m->common.memory;return 1;}
static int sequence_copy(void *v,uint64_t token,uint64_t d)
{
    struct sequence_model *m=v;unsigned p=m->run->picture_index;
    assert(token==42&&d==100000&&!m->decoding&&!m->common.lock&&p<2);
    if(m->variant==3&&p==1)return -1;
    if(++m->copies[p]<2)return 0;
    assert(p==m->completed);++m->completed;return 1;
}
static int sequence_read(void *v,uintptr_t address,uint32_t *out)
{
    struct sequence_model *m=v;
    if(address==0x07090160){*out=0x10000002;return 1;}
    if(address==VE+0x228&&m->decoding){
        ++m->reads;
        if(m->variant==1&&m->run->picture_index==1)*out=3;
        else *out=1|(m->reads<3?1u<<23:0);
        return 1;
    }
    if(address==VE+0x22c){*out=m->variant==2?5:6;return 1;}
    return read_reg(v,address,out);
}
static int sequence_write(void *v,uintptr_t address,unsigned width,uint32_t value)
{
    struct sequence_model *m=v;
    if(address==VE+0x224){
        if(value==7){assert(!m->decoding);m->flush=0;}
        else if((value&255)==3)m->flush+=value>>8;
        else if(value==8){
            unsigned p=m->run->picture_index;
            assert(p<2&&!m->decoding&&p==m->completed&&m->flush==m->run->picture.header_bits);
            memcpy(m->registers[p],m->common.ve,4096);memcpy(m->registers[p]+1024,m->sram,4096);
            m->decoding=1;m->reads=0;++m->triggers;
        }
    }
    if(address==VE+0x228){assert(m->decoding&&value==1&&m->reads>=3);m->decoding=0;}
    if(address==VE+0x2e0)m->sram_offset=value/4;
    if(address==VE+0x2e4){assert(m->sram_offset<1024);m->sram[m->sram_offset++]=value;}
    int result=write_reg(v,address,width,value);
    if(address==VE+0x228)m->common.ve[0x228/4]=0; /* W1C status model. */
    return result;
}
int main(int argc,char **argv)
{
    assert(argc==3);uint8_t bytes[A733_H264_INPUT_BYTES];FILE *f=fopen(argv[1],"rb");assert(f);
    size_t n=fread(bytes,1,sizeof(bytes),f);assert(n&&fgetc(f)==EOF&&!fclose(f));
    struct a733_h264_sequence parsed;assert(a733_h264_parse_sequence(bytes,n,&parsed)&&parsed.count==2);
    assert(parsed.pictures[0].poc==0&&parsed.pictures[1].poc==2&&parsed.pictures[1].frame_num==1);
    for(unsigned variant=0;variant<4;++variant){
        struct sequence_model m={.variant=variant};setup(&m.common,UINT64_C(0x3331000021320));
        m.common.ve[0x228/4]=m.common.ve[0xb0/4]=0;
        struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,now,&m));
        struct a733_gpu_resource_manager power={0};struct a733_gpu_resource_ops po={now,resource_read,unused_update,unused_rail};
        assert(a733_gpu_resources_init(&power,&po,&m));uint64_t pt;struct a733_gpu_power_resources ready;
        assert(a733_gpu_resources_acquire(&power,2,9,100000,&pt,&ready)==A733_GPU_RESOURCE_OK);
        struct a733_video_platform_config c={.clocks=&clocks,.power=&power,.power_client=2,.owner=99,.handoff=123,
            .pck={pt,9,A733_GPU_RESOURCE_STABLE},.module=0x80000001,.module_hz=234000000,
            .fabric={0,0x80000000,0x80000000},.fabric_hz={24000000,24000000,24000000}};
        a733_clock_mask_t scopes;assert(a733_video_platform_scopes(&c,&scopes));
        assert(a733_clock_acquire(&clocks,99,scopes,A733_CLOCK_VE_DEC,100000,&c.clock)==A733_CLOCK_OK);
        struct a733_h264_platform_ops ops={now,sequence_admit,sequence_memory,sequence_copy,enter,leave,sequence_read,sequence_write};
        struct a733_h264_platform platform={0};assert(a733_h264_platform_init(&platform,&c,&ops,&m));
        struct a733_h264_job job={.input=bytes,.input_bytes=n};
        const uint64_t bases[]={0x10001000,0x10010000,0x10014000,0x10020000,0x10068000,0x10070000};
        const uint64_t sizes[]={8192,8192,8192,0x21000,8192,0xc000};
        for(unsigned i=0;i<A733_H264_BUFFERS;++i)job.buffers[i]=(struct a733_h264_span){bases[i],sizes[i]};
        struct a733_h264_run run={0};m.run=&run;
        assert(a733_h264_begin(&run,a733_video_platform_h264_ops(),&platform,&job,100000));
        int result=0;for(unsigned p=0;!result&&p<10000;++p)result=a733_h264_poll(&run);
        assert(result==(variant?-1:1)&&!m.common.drain);
        assert(m.triggers==(variant==2?1:2)&&m.completed==(variant==2?0:variant?1:2));
        assert(a733_clock_token_status(&clocks,&c.clock)==A733_CLOCK_OK);
        if(!variant){
            assert(m.common.memory==1);f=fopen(argv[2],"wb");assert(f);
            assert(fwrite(m.registers,1,sizeof(m.registers),f)==sizeof(m.registers)&&!fclose(f));
        }
        printf("PASS sequence variant%u: triggers%u copied%u retained\n",variant,m.triggers,m.completed);
    }
}
