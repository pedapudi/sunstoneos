/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (c) 2013 Jens Kuske <jenskuske@gmail.com>
 * Copyright (c) 2018 Bootlin */
/* Bounded operation adapted from Linux Cedrus70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56
 * cedrus_h264.c (Jens Kuske/Bootlin), and checked against pinned A733 instructions.
 * Source/license identities are recorded in apps/h264/sources.json. */
#include "a733_video_h264.h"
#include "a733_h264_high.h"
#include <string.h>
/* Syntax adaptation from H6-CedarC h264_nalu.c, revision recorded in
 * apps/h264/sources.json (GNU Lesser General Public License version2).
 * Copyright (c) 2007-2020 Allwinnertech Co., Ltd.
 * This software is licensed under the terms of the GNU Lesser General Public
 * License version2, as published by the Free Software Foundation, and may be
 * copied, distributed, and modified under those terms. CedarC is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 * Register packing follows GPL Cedrus cedrus_set_params and A733 trace. */
struct bits { const uint8_t *p; size_t n, bit; int bad; };
static uint32_t get(struct bits *b,unsigned n)
{
    uint32_t v=0;
    if(n>32||b->bit>b->n*8||n>b->n*8-b->bit){b->bad=1;return 0;}
    while(n--){v=(v<<1)|((b->p[b->bit/8]>>(7-b->bit%8))&1);++b->bit;}
    return v;
}
static uint32_t ue(struct bits *b)
{
    unsigned n=0;
    while(!b->bad&&!get(b,1))if(++n>30){b->bad=1;return 0;}
    if(b->bad)return 0;
    return ((UINT32_C(1)<<n)-1)+get(b,n);
}
static int32_t se(struct bits *b)
{uint32_t v=ue(b);return (v&1)?(int32_t)(v/2+1):-(int32_t)(v/2);}
static int trailing(struct bits *b)
{
    if(get(b,1)!=1)return 0;
    while(b->bit%8)if(get(b,1))return 0;
    return !b->bad&&b->bit==b->n*8;
}
static int vui(struct bits *b)
{
    if(get(b,1)){
        uint32_t aspect=get(b,8);
        if(aspect==255){if(!get(b,16)||!get(b,16))return 0;}
        else if(aspect>16)return 0;
    }
    if(get(b,1))get(b,1);
    if(get(b,1)){get(b,3);get(b,1);if(get(b,1)){get(b,8);get(b,8);get(b,8);}}
    if(get(b,1)){if(ue(b)>5||ue(b)>5)return 0;}
    if(get(b,1)){if(!get(b,32)||!get(b,32))return 0;get(b,1);}
    /* HRD scheduling has no consumer in this one-picture mode. */
    if(get(b,1)||get(b,1))return 0;
    get(b,1);
    if(get(b,1)){
        get(b,1);
        if(ue(b)>16||ue(b)>16||ue(b)>16||ue(b)>16||ue(b)>1||ue(b)>1)return 0;
    }
    return !b->bad;
}
/* Unescape and validate each complete NAL before reading syntax. The buffer is
 * bounded by the padded input allocation; never append or scan past its end. */
static int rbsp(const uint8_t *p,size_t n,uint8_t *out,size_t capacity,size_t *count)
{
    size_t used=0;unsigned zeros=0;
    for(size_t i=0;i<n;++i){
        uint8_t v=p[i];
        if(zeros==2){
            if(v==3){if(i+1==n||p[i+1]>3)return 0;zeros=0;continue;}
            if(v<3)return 0;
        }
        if(used<capacity)out[used]=v;++used;zeros=v==0?zeros+1:0;
    }
    *count=used<capacity?used:capacity;return used!=0;
}
static size_t startcode(const uint8_t *p,size_t n,size_t at)
{
    if(n-at>=4&&!p[at]&&!p[at+1]&&!p[at+2]&&p[at+3]==1)return 4;
    if(n-at>=3&&!p[at]&&!p[at+1]&&p[at+2]==1)return 3;
    return 0;
}
static int parse_sequence(const uint8_t *p,size_t n,struct a733_h264_sequence *out,
                          unsigned vga,unsigned frame_num,size_t *config_bytes)
{
    struct a733_h264_picture picture={0};
    struct a733_h264_sequence sequence={0};
    uint32_t sid=0,pid=0,frame_bits=0,poc=0,poc_bits=0,bottom=0;
    uint32_t deblock=0,redundant=0,l0=0,l1=0,refs=0,weighted=0;
    int32_t init_qp=0,chroma=0;
    uint8_t unescaped[A733_H264_SLOT_BYTES];
    unsigned stage=0;
    size_t bound=vga==2?A733_H264_FULLHD_INPUT_BYTES:vga?A733_H264_VGA_INPUT_BYTES:A733_H264_INPUT_BYTES;
    if(!p||!out||!n||n>bound)return 0;
    for(size_t at=0;at<n;){
        size_t prefix=startcode(p,n,at);if(!prefix)return 0;
        size_t begin=at+prefix,end=begin;
        while(end<n&&!startcode(p,n,end))++end;
        /* trailing_zero_8bits are AnnexB framing, outside the NAL payload. */
        size_t finish=end;while(finish>begin&&!p[finish-1])--finish;
        if(finish-begin<2||finish-begin>(vga?bound:A733_H264_SLOT_BYTES)||(p[begin]&0x80))return 0;
        unsigned type=p[begin]&31,ref=(p[begin]>>5)&3;
        size_t bytes;
        if(vga&&((stage==0&&type!=7)||(stage==1&&type!=8)||
           (stage==2&&type!=(frame_num?1u:5u))||stage==3))return 0;
        if(type!=1&&type!=5&&finish-begin>A733_H264_SLOT_BYTES)return 0;
        if(!rbsp(p+begin+1,finish-begin-1,unescaped,sizeof(unescaped),&bytes))return 0;
        struct bits b={unescaped,bytes,0,0};
        if(type==6&&stage<=3){
            if(ref)return 0; /* SEI metadata does not alter decoded samples. */
        }else if(type==9&&(stage==0||stage==3)){
            if(ref||get(&b,3)>7||!trailing(&b))return 0;
        }else if(type==7&&stage==0){
            if(!ref||get(&b,8)!=66)return 0;
            if(get(&b,8)&3)return 0;
            uint32_t level=get(&b,8);if(!level||level>51||(vga==2&&level!=40))return 0;
            sid=ue(&b);if(sid>31)return 0;
            frame_bits=ue(&b);if(frame_bits>12)return 0;frame_bits+=4;
            poc=ue(&b);if(poc==0){poc_bits=ue(&b);if(poc_bits>12)return 0;poc_bits+=4;}
            else if(poc!=2)return 0;
            refs=ue(&b);if(refs>1||get(&b,1))return 0;
            uint32_t w=ue(&b),h=ue(&b);
            if(vga==2?(w!=119||h!=67):vga?(w!=39||h!=29):(w>3||h>1))return 0;
            if(get(&b,1)!=1)return 0;
            uint32_t direct=get(&b,1);
            if(vga&&(!direct||poc!=2||refs!=1||frame_bits!=4))return 0;
            uint32_t crop=get(&b,1);
            if(vga==2){
                if(!crop||ue(&b)||ue(&b)||ue(&b)||ue(&b)!=4)return 0;
                picture.crop_bottom=8;
            }else if(crop)return 0;
            if(get(&b,1)&&!vui(&b))return 0;
            if(!trailing(&b))return 0;
            picture.width=(w+1)*16;picture.height=(h+1)*16;
            picture.visible_width=picture.width;picture.visible_height=picture.height-picture.crop_bottom;
            picture.sps=0xc0000|(direct<<16)|(w<<8)|h;
            stage=1;
        }else if(type==8&&stage==1){
            if(!ref)return 0;
            pid=ue(&b);if(pid>255||ue(&b)!=sid||get(&b,1))return 0;
            bottom=get(&b,1);if(ue(&b))return 0;
            l0=ue(&b);l1=ue(&b);if(l0>31||l1>31)return 0;
            weighted=get(&b,1);uint32_t bipred=get(&b,2);if(bipred>2)return 0;
            init_qp=se(&b);int32_t qs=se(&b);chroma=se(&b);
            if(init_qp < -26||init_qp>25||qs < -26||qs>25||chroma < -12||chroma>12)return 0;
            deblock=get(&b,1);uint32_t constrained=get(&b,1);redundant=get(&b,1);
            if(vga&&(bottom||l0||l1||weighted||bipred||redundant))return 0;
            if(!trailing(&b))return 0; /* Baseline PPS has no extension. */
            picture.pps=(l0<<10)|(l1<<5)|(weighted<<4)|(bipred<<2)|(constrained<<1);
            stage=2;if(config_bytes)*config_bytes=end;
        }else if((type==5&&stage==2)||(type==1&&(stage==3||(vga&&stage==2)))){
            unsigned is_p=type==1;
            if(sequence.count>=A733_H264_PICTURES||!ref||ue(&b))return 0;
            uint32_t slice=ue(&b);
            if((is_p?(slice!=0&&slice!=5):(slice!=2&&slice!=7))||ue(&b)!=pid)return 0;
            picture.frame_num=get(&b,frame_bits);picture.slice_type=is_p?0:2;
            if(picture.frame_num!=(vga?frame_num:is_p))return 0;
            if(!is_p&&ue(&b)>65535)return 0;
            picture.poc=0;
            if(poc==0){picture.poc=get(&b,poc_bits);if(bottom&&se(&b))return 0;}
            if(redundant&&ue(&b))return 0;
            uint32_t override=0;
            if(is_p){
                /* Adapted CedarC reference defaults/reordering/marking order.
                 * With one short-term reference and no wrap, default list0 is
                 * the retained IDR. POC type2 is2*frame_num for reference frames. */
                if(poc!=2||refs!=1||l0||weighted||redundant)return 0;
                override=get(&b,1);if(override&&ue(&b))return 0;
                if(get(&b,1))return 0; /* ref_pic_list_modification_flag_l0 */
                if(get(&b,1))return 0; /* adaptive_ref_pic_marking_mode_flag */
                picture.poc=2*picture.frame_num;
            }else{uint32_t no_output=get(&b,1);if((vga&&no_output)||get(&b,1))return 0;}
            int32_t delta=se(&b),q=26+init_qp+delta;
            if(q<0||q>51)return 0;
            uint32_t disable=0;int32_t alpha=0,beta=0;
            if(deblock){disable=ue(&b);if(disable>2)return 0;
                if(disable!=1){alpha=se(&b);beta=se(&b);if(alpha < -6||alpha>6||beta < -6||beta>6)return 0;}}
            if(b.bad||b.bit>=bytes*8)return 0;
            picture.slice_offset=(uint32_t)begin;picture.slice_bytes=(uint32_t)(finish-begin);
            picture.header_bits=(uint32_t)b.bit+8;
            picture.shs=0x1020|(is_p?0:0x200);
            /* As in the accepted A733 trace, I slices use PPS reference defaults
             * and do not assert the active-reference override flag. */
            picture.shs2=(override<<12)|(disable<<8)|(((uint32_t)alpha&15)<<4)|((uint32_t)beta&15);
            picture.qp=0x1000000|(((uint32_t)chroma&63)<<16)|(((uint32_t)chroma&63)<<8)|(uint32_t)q;
            if(picture.slice_bytes>(vga?bound:A733_H264_SLOT_BYTES))return 0;
            sequence.pictures[sequence.count++]=picture;
            stage=3;
        }else return 0;
        at=end;
    }
    if(stage!=3||!sequence.count)return 0;
    *out=sequence;return 1;
}
int a733_h264_parse_sequence(const uint8_t *p,size_t n,struct a733_h264_sequence *out)
{return parse_sequence(p,n,out,0,0,NULL);}
int a733_h264_parse_stream(enum a733_h264_profile profile,const uint8_t *p,size_t n,unsigned frame_num,
                        struct a733_h264_picture *out,size_t *config_bytes)
{
    struct a733_h264_sequence sequence;size_t config=0;
    if((profile!=A733_H264_VGA_STREAM&&profile!=A733_H264_FULLHD_STREAM)||!out||!config_bytes||frame_num>15||
       !parse_sequence(p,n,&sequence,(unsigned)profile,frame_num,&config)||sequence.count!=1)return 0;
    *out=sequence.pictures[0];*config_bytes=config;return 1;
}
int a733_h264_parse_vga(const uint8_t *p,size_t n,unsigned frame_num,
                        struct a733_h264_picture *out,size_t *config_bytes)
{return a733_h264_parse_stream(A733_H264_VGA_STREAM,p,n,frame_num,out,config_bytes);}
int a733_h264_parse(const uint8_t *p,size_t n,struct a733_h264_picture *out)
{
    struct a733_h264_sequence sequence;
    if(!out||!a733_h264_parse_sequence(p,n,&sequence)||sequence.count!=1)return 0;
    *out=sequence.pictures[0];return 1;
}
static int fail(struct a733_h264_run *r){r->state=A733_H264_FAULT;return -1;}
static int live(struct a733_h264_run *r)
{
    uint64_t now;
    if(r->ops.now_us(r->context,&now)!=1||now<r->last_now||now>=r->deadline)return fail(r);
    r->last_now=now;return 1;
}
static int read_reg(struct a733_h264_run *r,uint32_t off,uint32_t *out)
{return live(r)==1&&r->ops.read(r->context,r->token,off,out,r->deadline)==1&&live(r)==1?1:fail(r);}
static int write_reg(struct a733_h264_run *r,uint32_t off,uint32_t value)
{return live(r)==1&&r->ops.write(r->context,r->token,off,value,r->deadline)==1&&live(r)==1?1:fail(r);}
static int begin(struct a733_h264_run *r,const struct a733_h264_ops *o,void *v,
                    const struct a733_h264_job *j,uint64_t d)
{
    static const uint64_t sizes[4][A733_H264_BUFFERS]={
        {8192,8192,8192,0x21000,8192,0xc000},
        {524288,614400,311296,0x22000,81920,0xc000},
        {4194304,4177920,2088960,0x35000,524288,0xc000},
        {4194304,6266880,3133440,0x35000,786432,0xc000}};
    if(!j||j->profile>A733_H264_HIGH_STREAM||(int)j->profile<0)return 0;
    const uint64_t *capacity=sizes[j->profile];
    if(!r||r->state!=A733_H264_NEW||!o||!j||!j->input||!d||!o->now_us||
       !j->input_bytes||j->input_bytes>(j->profile>=A733_H264_FULLHD_STREAM?A733_H264_FULLHD_INPUT_BYTES:j->profile?A733_H264_VGA_INPUT_BYTES:A733_H264_INPUT_BYTES)||!o->acquire||!o->prepare||!o->read||!o->write||!o->copied)return 0;
    for(unsigned i=0;i<A733_H264_BUFFERS;++i){
        const struct a733_h264_span *s=&j->buffers[i];
        if(!s->iova||(s->iova&4095)||s->bytes!=capacity[i]||
           s->iova>UINT64_C(0x100000000)-s->bytes||
           (i==A733_H264_NEIGHBOR&&(s->iova&16383)))return 0;
        for(unsigned k=0;k<i;++k)if(s->iova<j->buffers[k].iova+j->buffers[k].bytes&&
            j->buffers[k].iova<s->iova+s->bytes)return 0;
    }
    *r=(struct a733_h264_run){.ops=*o,.job=*j,.context=v,.deadline=d,.state=A733_H264_ACQUIRE};
    return 1;
}
int a733_h264_begin(struct a733_h264_run *r,const struct a733_h264_ops *o,void *v,
                    const struct a733_h264_job *j,uint64_t d)
{return j&&j->profile==A733_H264_LEGACY?begin(r,o,v,j,d):0;}
int a733_h264_stream_begin(struct a733_h264_run *r,const struct a733_h264_ops *o,void *v,
    const struct a733_h264_job *j,size_t bytes,uint64_t d)
{
    struct a733_h264_picture picture;size_t config;
    if(!j||(j->profile!=A733_H264_VGA_STREAM&&j->profile!=A733_H264_FULLHD_STREAM)||
       j->input_bytes!=(j->profile>=A733_H264_FULLHD_STREAM?A733_H264_FULLHD_INPUT_BYTES:A733_H264_VGA_INPUT_BYTES)||
       !a733_h264_parse_stream(j->profile,j->input,bytes,0,&picture,&config)||!begin(r,o,v,j,d))return 0;
    r->picture=picture;r->sequence.pictures[0]=picture;r->frame_bytes=bytes;return 1;
}
int a733_h264_stream_next(struct a733_h264_run *r,size_t bytes,unsigned frame_num)
{
    struct a733_h264_picture picture;size_t config;
    if(!r||(r->job.profile!=A733_H264_VGA_STREAM&&r->job.profile!=A733_H264_FULLHD_STREAM)||r->state!=A733_H264_COPIED)return 0;
    if(r->picture_index>=255||(frame_num&&frame_num!=r->picture.frame_num+1)||
       !a733_h264_parse_stream(r->job.profile,r->job.input,bytes,frame_num,&picture,&config)||live(r)!=1)return fail(r);
    ++r->picture_index;r->picture=picture;r->sequence.pictures[r->picture_index&1]=picture;
    r->frame_bytes=bytes;r->index=0;r->flushed=0;r->flush_bits=0;r->state=A733_H264_CHECK;return 1;
}
int a733_h264_vga_begin(struct a733_h264_run *r,const struct a733_h264_ops *o,void *v,
    const struct a733_h264_job *j,size_t bytes,uint64_t d)
{return j&&j->profile==A733_H264_VGA_STREAM?a733_h264_stream_begin(r,o,v,j,bytes,d):0;}
int a733_h264_vga_next(struct a733_h264_run *r,size_t bytes,unsigned frame_num)
{return r&&r->job.profile==A733_H264_VGA_STREAM?a733_h264_stream_next(r,bytes,frame_num):0;}
int a733_h264_high_begin(struct a733_h264_run *r,const struct a733_h264_ops *o,void *v,
    const struct a733_h264_job *j,size_t bytes,const struct h264_high_plan *p,uint64_t deadline)
{
    if(!j||j->profile!=A733_H264_HIGH_STREAM||j->input_bytes!=4194304||!bytes||bytes>j->input_bytes||!p||!p->slots||p->slots>5||p->slot>=p->slots||!begin(r,o,v,j,deadline))return 0;
    r->high=p;r->picture=p->picture;r->frame_bytes=bytes;return 1;
}
int a733_h264_high_next(struct a733_h264_run *r,size_t bytes,const struct h264_high_plan *p)
{
    if(!r||r->job.profile!=A733_H264_HIGH_STREAM||r->state!=A733_H264_COPIED||r->picture_index>=255||!p||!bytes||bytes>r->job.input_bytes||p->slot>=p->slots||p->slots>5||live(r)!=1)return 0;
    ++r->picture_index;r->high=p;r->picture=p->picture;r->frame_bytes=bytes;
    r->index=r->flushed=r->flush_bits=0;r->state=A733_H264_CHECK;return 1;
}
static int program_high(struct a733_h264_run *r)
{
    const struct h264_high_plan *p=r->high;const struct a733_h264_span *b=r->job.buffers;
    unsigned n=r->index;uint32_t off,value;
    static const uint32_t offsets[]={0xec,0xc4,0xc8,0x250,0x254,0x240,0x22c,0x260,0x24c,0x200,0x204,0x208,0x20c,0x21c,0x220,0x234,0x238,0x23c,0x230,0x2e0};
    uint32_t input=(uint32_t)b[0].iova;
    uint32_t values[]={r->format,p->picture.width*p->picture.height/4,(p->picture.width/2<<16)|p->picture.width,(uint32_t)b[3].iova,(uint32_t)b[5].iova,0,0,0,p->slot,p->picture.sps,p->picture.pps,p->picture.shs,p->picture.shs2,p->picture.qp,0x407,0,p->picture.slice_bytes*8,input+4194303,(input&0x0ffffff0)|(input>>28)|0x70000000,0x400};
    unsigned count=sizeof(offsets)/sizeof(*offsets);
    if(n<count){off=offsets[n];value=values[n];}
    else if((n-=count)<18*8){
        unsigned slot=n/8,field=n%8;off=0x2e4;value=0;
        if(slot<5&&p->valid[slot]){
            if(field<2)value=p->pocs[slot][field];
            if(field==2&&slot==p->slot&&!p->reference)value=0x22;
            if(field==3)value=(uint32_t)b[1].iova+slot*p->y_step;
            if(field==4)value=(uint32_t)b[2].iova+slot*p->c_step;
            if(field==5||field==6)value=(uint32_t)b[4].iova+slot*p->mv_step+(field==6?p->mv_field:0);
        }
    }else if((n-=18*8)<57){off=n?0x2e4:0x2e0;value=n?p->scaling[n-1]:0x800;}
    else return 0;
    if(write_reg(r,off,value)!=1)return -1;++r->index;return 1;
}
static int high_tables(struct a733_h264_run *r)
{
    const struct h264_high_plan *p=r->high;unsigned n=r->index;uint32_t off,value;
    if(!n){off=0x210;value=p->weight_denom;}
    else if(--n<20){unsigned list=n/10,word=n%10;off=word?0x2e4:0x2e0;value=word?0:0x640+list*36;
        if(word&&word<=8)for(unsigned j=0;j<4;++j){unsigned i=(word-1)*4+j;if(i<p->ref_count[list])value|=(p->lists[list][i]<<1)<<(8*j);}
    }else if((n-=20)<193){off=n?0x2e4:0x2e0;value=n?p->weights[n-1]:0;}
    else return 0;
    if(write_reg(r,off,value)!=1)return -1;++r->index;return 1;
}
/* Program provenance: SPS/PPS/SHS/QP/VLD and frameSRAM match the preserved
 * libawh264 instruction trace. Initial full frame-list clearing, primary-only
 * buffers and offset0/INIT/FLUSH come from Cedrus. SDROT_CTRL0 disables scaling;
 * no secondary-output addresses or scaling triggers are programmed.
 */
static int program(struct a733_h264_run *r)
{
    if(r->job.profile==A733_H264_HIGH_STREAM)return program_high(r);
    const struct a733_h264_span *s=r->job.buffers;
    unsigned vga=r->job.profile!=A733_H264_LEGACY,hd=r->job.profile==A733_H264_FULLHD_STREAM;
    uint32_t off,value,n=r->index,slot=vga?(r->picture_index&1):r->picture_index;
    uint32_t input=(uint32_t)s[A733_H264_INPUT].iova+(vga?0:slot*4096);
    static const uint32_t offsets[]={0xec,0xc4,0xc8,0x250,0x254,0x240,0x22c,0x260,
        0x24c,0x200,0x204,0x208,0x20c,0x21c,0x220,0x234,0x238,0x23c,0x230,0x2e0};
    uint32_t values[]={r->format,hd?522240:vga?76800:512,hd?0x03c00780:vga?0x01400280:0x00200040,
        (uint32_t)s[A733_H264_PICTURE_INFO].iova,
        (uint32_t)s[A733_H264_NEIGHBOR].iova,0,0,0,slot,
        r->picture.sps,r->picture.pps,r->picture.shs,r->picture.shs2,
        r->picture.qp,0x407,0,r->picture.slice_bytes*8,input+(hd?A733_H264_FULLHD_INPUT_BYTES:vga?A733_H264_VGA_INPUT_BYTES:A733_H264_SLOT_BYTES)-1,
        (input&0x0ffffff0)|(input>>28)|0x70000000,0x400};
    unsigned count=sizeof(offsets)/sizeof(offsets[0]);
    if(n<count){off=offsets[n];value=values[n];}
    else if((n-=count)<18*8){
        /* Cedrus framebuffer SRAM layout. Each picture keeps independent DMA
         * extents for the entire session, including both MV fields. */
        unsigned frame=n/8,field=n%8;off=0x2e4;value=0;
        if(vga?(frame==slot||(r->picture.frame_num&&frame==(slot^1))):(frame<=slot)){
            if(field<2)value=r->sequence.pictures[frame].poc;
            if(field==3)value=(uint32_t)s[A733_H264_Y].iova+frame*(hd?2088960:vga?307200:4096);
            if(field==4)value=(uint32_t)s[A733_H264_C].iova+frame*(hd?1044480:vga?155648:4096);
            if(field==5||field==6)value=(uint32_t)s[A733_H264_MV].iova+frame*(hd?262144:vga?40960:4096)+(field==6?(hd?131072:vga?19456:1024):0);
        }
    }else{
        n-=18*8;
        /* List0 contains one progressive IDR reference at slot0. Clear the
         * entire32-entry list so no inherited bytes can select another slot. */
        if(n>8)return 0;
        off=n?0x2e4:0x2e0;value=n?((vga&&r->picture.frame_num&&n==1)?(slot^1)*2:0):0x640;
    }
    if(write_reg(r,off,value)!=1)return -1;
    ++r->index;return 1;
}
int a733_h264_poll(struct a733_h264_run *r)
{
    if(!r||r->state==A733_H264_NEW)return -1;
    if(r->state==A733_H264_FAULT)return -1;
    if(r->state==A733_H264_COPIED)return 1;
    if(live(r)!=1)return -1;
    int result;uint32_t value;
    switch(r->state){
    case A733_H264_ACQUIRE:{
        uint64_t previous=r->token;
        result=r->ops.acquire(r->context,&r->job,r->deadline,&r->token);
        if(!r->token||(previous&&previous!=r->token)||(result!=0&&result!=1)||live(r)!=1)return fail(r);
        if(result)r->state=A733_H264_HASH;
        break;}
    case A733_H264_HASH:{
        if(r->job.profile==A733_H264_LEGACY){
            if(!a733_h264_parse_sequence(r->job.input,r->job.input_bytes,&r->sequence)||live(r)!=1)return fail(r);
            r->picture=r->sequence.pictures[0];
        }
        r->state=A733_H264_IDENTIFY;r->index=0;break;}
    case A733_H264_IDENTIFY:
        if(read_reg(r,r->index==0?0xf0:r->index==1?0xe0:0xe4,&value)!=1)return -1;
        if(!r->index&&value&&value!=0x16670040)return fail(r);
        if(r->index==1)r->identity_high=value&0xfffff;
        if(++r->index==3){
            r->version=((uint64_t)r->identity_high<<32)|(value&0xfffff);
            if(r->version!=UINT64_C(0x3332100021321)&&r->version!=UINT64_C(0x3331000021320)&&
               r->version!=UINT64_C(0x3052000012011))return fail(r);
            r->state=A733_H264_PREPARE;r->index=0;
        }break;
    case A733_H264_PREPARE:
        result=r->ops.prepare(r->context,r->token,&r->job,r->deadline);
        if((result!=0&&result!=1)||live(r)!=1)return fail(r);
        if(result)r->state=A733_H264_CHECK;
        break;
    case A733_H264_CHECK:{
        static const uint32_t offsets[]={0,0x1c,0xb0,0xec,0x228,0x5c,0x804};
        if(read_reg(r,offsets[r->index],&value)!=1)return -1;
        if((r->index==0&&(value&0x2c00101f)!=1)||
           (r->index==1&&(value&0x100))||(r->index==2&&(value&0x80000000))||
           (r->index==4&&(value&(A733_H264_BUSY_MASK|15)))||
           (r->index==5&&(value&0x10))||(r->index==6&&(value&0x11)))return fail(r);
        if(r->index==3)r->format=(value&~0x77u)|0x20;
        if(++r->index==7){r->index=0;r->state=A733_H264_PROGRAM;}break;}
    case A733_H264_PROGRAM:
        result=program(r);if(result<0)return -1;
        if(!result)r->state=A733_H264_INIT;
        break;
    case A733_H264_INIT:
        if(write_reg(r,0x224,7)!=1)return -1;
        r->state=A733_H264_FLUSH;break;
    case A733_H264_FLUSH:
        r->flush_bits=r->picture.header_bits-r->flushed;
        if(r->flush_bits>32)r->flush_bits=32;
        if(write_reg(r,0x224,(r->flush_bits<<8)|3)!=1)return -1;
        r->state=A733_H264_HEADER_WAIT;break;
    case A733_H264_HEADER_WAIT:
        if(read_reg(r,0x228,&value)!=1)return -1;
        if(value&0xe)return fail(r);
        if(!(value&0x100)){
            r->flushed+=r->flush_bits;
            if(r->flushed==r->picture.header_bits&&r->job.profile==A733_H264_HIGH_STREAM){r->index=0;r->state=A733_H264_REFERENCE_TABLES;}
            else r->state=r->flushed==r->picture.header_bits?A733_H264_TRIGGER:A733_H264_FLUSH;
        }
        break;
    case A733_H264_REFERENCE_TABLES:
        result=high_tables(r);if(result<0)return -1;if(!result)r->state=A733_H264_TRIGGER;break;
    case A733_H264_TRIGGER:
        if(write_reg(r,0x224,8)!=1)return -1;
        r->state=A733_H264_WAIT;break;
    case A733_H264_WAIT:
        if(read_reg(r,0x228,&r->status)!=1)return -1;
        if(r->status&0xe)return fail(r);
        if((r->status&1)&&!(r->status&A733_H264_BUSY_MASK))r->state=A733_H264_MB_COUNT;
        break;
    case A733_H264_MB_COUNT:
        if(read_reg(r,0x22c,&value)!=1)return -1;
        if(value!=(r->picture.width/16)*(r->picture.height/16))return fail(r);
        r->state=A733_H264_DISABLE;break;
    case A733_H264_DISABLE:
        if(write_reg(r,0x220,0x400)!=1)return -1;
        r->state=A733_H264_ACK;break;
    case A733_H264_ACK:
        if(write_reg(r,0x228,r->status&7)!=1)return -1;
        r->state=A733_H264_COPY;break;
    case A733_H264_COPY:
        result=r->ops.copied(r->context,r->token,r->deadline);
        if((result!=0&&result!=1)||live(r)!=1)return fail(r);
        if(result){
            if(r->job.profile!=A733_H264_LEGACY){r->state=A733_H264_COPIED;return 1;}
            if(++r->picture_index==r->sequence.count){r->state=A733_H264_COPIED;return 1;}
            r->picture=r->sequence.pictures[r->picture_index];
            r->index=0;r->flushed=0;r->flush_bits=0;r->state=A733_H264_CHECK;
        }
        break;
    default:return fail(r);
    }
    return 0;
}
