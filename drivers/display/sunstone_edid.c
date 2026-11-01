/* SPDX-License-Identifier: GPL-2.0-or-later */
/* EDID and DisplayID interpretation follows the Linux DRM implementation.
 * Copyright (c) 2006 Luc Verhaegen (quirks list)
 * Copyright (c) 2007-2008 Intel Corporation
 * Copyright (c) 2010 Red Hat, Inc.
 * No Linux runtime interfaces are used. */
#include "sunstone_display.h"
#include "a733_range.h"
#include <string.h>
struct cta_mode { uint8_t vic; struct sunstone_display_mode mode; };
#define PP (SUNSTONE_MODE_HSYNC_POSITIVE|SUNSTONE_MODE_VSYNC_POSITIVE)
/* Exact integer CTA rates only. Other VICs remain visible as skipped timings. */
static const struct cta_mode cta[] = {
 {1,{25175000,640,480,16,96,48,10,2,33,0}},
 {2,{27000000,720,480,16,62,60,9,6,30,0}},
 {3,{27000000,720,480,16,62,60,9,6,30,0}},
 {4,{74250000,1280,720,110,40,220,5,5,20,PP}},
 {16,{148500000,1920,1080,88,44,148,4,5,36,PP}},
 {17,{27000000,720,576,12,64,68,5,5,39,0}},
 {18,{27000000,720,576,12,64,68,5,5,39,0}},
 {19,{74250000,1280,720,440,40,220,5,5,20,PP}},
 {31,{148500000,1920,1080,528,44,148,4,5,36,PP}},
 {32,{74250000,1920,1080,638,44,148,4,5,36,PP}},
 {33,{74250000,1920,1080,528,44,148,4,5,36,PP}},
 {34,{74250000,1920,1080,88,44,148,4,5,36,PP}},
 {93,{297000000,3840,2160,1276,88,296,8,10,72,PP}},
 {94,{297000000,3840,2160,1056,88,296,8,10,72,PP}},
 {95,{297000000,3840,2160,176,88,296,8,10,72,PP}},
 {96,{594000000,3840,2160,1056,88,296,8,10,72,PP}},
 {97,{594000000,3840,2160,176,88,296,8,10,72,PP}},
};
static unsigned le16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static int checksum(const uint8_t *p,size_t n)
{unsigned sum=0;for(size_t i=0;i<n;++i)sum+=p[i];return !(sum&255);}
static int add(struct sunstone_display_modes *out,struct sunstone_display_mode m)
{
    if(!sunstone_display_mode_valid(&m))return 0;
    for(size_t i=0;i<out->count;++i)if(sunstone_display_mode_equal(&out->modes[i],&m)){
        out->modes[i].flags|=m.flags&SUNSTONE_MODE_PREFERRED;return 1;
    }
    if(out->count==SUNSTONE_DISPLAY_MAX_MODES){out->truncated=1;return 1;}
    out->modes[out->count++]=m;return 1;
}
static int dtd(const uint8_t *p,struct sunstone_display_modes *out,unsigned preferred)
{
    if(!le16(p))return 1; /* Monitor descriptor or padding. */
    unsigned h=p[2]|((p[4]&0xf0)<<4),hb=p[3]|((p[4]&15)<<8);
    unsigned v=p[5]|((p[7]&0xf0)<<4),vb=p[6]|((p[7]&15)<<8);
    unsigned hf=p[8]|((p[11]&0xc0)<<2),hs=p[9]|((p[11]&0x30)<<4);
    unsigned vf=(p[10]>>4)|((p[11]&12)<<2),vs=(p[10]&15)|((p[11]&3)<<4);
    /* Analog/composite synchronization has no admitted digital transport. */
    if((p[17]&0x18)!=0x18){++out->skipped_timings;return 1;}
    if(hb<hf+hs||vb<vf+vs)return 0;
    struct sunstone_display_mode m={le16(p)*10000u,h,v,hf,hs,hb-hf-hs,vf,vs,vb-vf-vs,
        (p[17]&2?SUNSTONE_MODE_HSYNC_POSITIVE:0)|(p[17]&4?SUNSTONE_MODE_VSYNC_POSITIVE:0)|
        (p[17]&128?SUNSTONE_MODE_INTERLACED:0)|(preferred?SUNSTONE_MODE_PREFERRED:0)};
    return add(out,m);
}
static int parse_cta(const uint8_t *p,struct sunstone_display_modes *out)
{
    if(p[1]<3)return 0;
    unsigned end=p[2];
    if(!end)return 1;
    if(end<4||end>127)return 0;
    for(unsigned i=4;i<end;){
        unsigned tag=p[i]>>5,n=p[i]&31;i++;
        if(n>end-i)return 0;
        if(tag==2)for(unsigned j=0;j<n;++j){
            unsigned vic=p[i+j]&127,found=0;
            for(size_t k=0;k<sizeof(cta)/sizeof(*cta);++k)if(cta[k].vic==vic){
                struct sunstone_display_mode m=cta[k].mode;
                if(p[i+j]&128)m.flags|=SUNSTONE_MODE_PREFERRED;
                if(!add(out,m))return 0;found=1;break;
            }
            if(!found)++out->skipped_timings;
        }
        if(tag==3&&n>=5&&p[i]==3&&p[i+1]==12&&p[i+2]==0){
            out->hdmi=1;
            if(n>=7&&p[i+6])out->max_tmds_hz=(uint32_t)p[i+6]*5000000;
        }
        if(tag==7&&n>=2&&p[i]==0&&(p[i+1]&0x40))out->rgb_range_selectable=1;
        i+=n;
    }
    unsigned native=p[3]&15,index=0;
    for(unsigned i=end;i+18<=127;i+=18,++index)if(!dtd(p+i,out,index<native))return 0;
    return 1;
}
static int displayid_timing(const uint8_t *p,unsigned type7,struct sunstone_display_modes *out)
{
    uint64_t clock=((uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16))+1u;
    clock*=type7?1000u:10000u;
    unsigned h=le16(p+4)+1,hb=le16(p+6)+1,hf=(le16(p+8)&0x7fff)+1,hs=le16(p+10)+1;
    unsigned v=le16(p+12)+1,vb=le16(p+14)+1,vf=(le16(p+16)&0x7fff)+1,vs=le16(p+18)+1;
    if(clock>UINT32_MAX||h>65535||v>65535||hb<hf+hs||vb<vf+vs||
       hs>65535||vs>65535||hb-hf-hs>65535||vb-vf-vs>65535)return 0;
    struct sunstone_display_mode m={(uint32_t)clock,h,v,hf,hs,hb-hf-hs,vf,vs,vb-vf-vs,
        (p[9]&128?SUNSTONE_MODE_HSYNC_POSITIVE:0)|(p[17]&128?SUNSTONE_MODE_VSYNC_POSITIVE:0)|
        (p[3]&128?SUNSTONE_MODE_PREFERRED:0)|(p[3]&0x10?SUNSTONE_MODE_INTERLACED:0)};
    return add(out,m);
}
static int parse_displayid(const uint8_t *p,struct sunstone_display_modes *out)
{
    unsigned end=5u+p[2];
    if(end>126||!checksum(p+1,end))return 0;
    if(p[1]!=0x12&&p[1]!=0x13&&p[1]!=0x20){++out->unknown_extensions;return 1;}
    for(unsigned i=5;i<end;){
        unsigned nonzero=0;for(unsigned j=i;j<end;++j)nonzero|=p[j];
        if(!nonzero)return 1; /* Real DisplayID sections may include zero padding. */
        if(end-i<3)return 0;
        unsigned tag=p[i],n=p[i+2];i+=3;
        if(n>end-i)return 0;
        if(tag==3||tag==0x22){
            if(n%20)return 0;
            for(unsigned j=0;j<n;j+=20)if(!displayid_timing(p+i+j,tag==0x22,out))return 0;
        }else if(tag==4||tag==5||tag==6||tag==0x23||tag==0x24||tag==0x25||tag==0x26)
            ++out->skipped_timings;
        i+=n;
    }
    return 1;
}
int sunstone_display_edid_parse(const uint8_t *e,size_t bytes,struct sunstone_display_modes *out)
{
    static const uint8_t signature[]={0,255,255,255,255,255,255,0};
    if(!e||!out||a733_ranges_overlap_or_wrap(e,bytes,out,sizeof(*out)))return SUNSTONE_EDID_ARGUMENT;
    if(bytes<128||bytes>SUNSTONE_DISPLAY_MAX_EDID||(bytes&127))return SUNSTONE_EDID_LENGTH;
    if(memcmp(e,signature,8)||e[18]!=1||(e[19]!=3&&e[19]!=4)||!(e[20]&128))return SUNSTONE_EDID_HEADER;
    if((size_t)(e[126]+1)*128!=bytes)return SUNSTONE_EDID_LENGTH;
    for(size_t i=0;i<bytes;i+=128)if(!checksum(e+i,128))return SUNSTONE_EDID_CHECKSUM;
    struct sunstone_display_modes modes={0};
    /* Standard/established timings need a timing formula or DMT table. The
     * bounded parser reports their presence instead of inventing porches. */
    if(e[35]||e[36]||e[37])++modes.skipped_timings;
    for(unsigned i=38;i<54;i+=2)if(e[i]!=1||e[i+1]!=1)++modes.skipped_timings;
    for(unsigned i=54;i+18<=126;i+=18)if(!dtd(e+i,&modes,i==54&&(e[24]&2)))return SUNSTONE_EDID_TIMING;
    for(size_t i=128;i<bytes;i+=128){
        if(e[i]==2){if(!parse_cta(e+i,&modes))return SUNSTONE_EDID_BLOCK;}
        else if(e[i]==0x70){if(!parse_displayid(e+i,&modes))return SUNSTONE_EDID_BLOCK;}
        else ++modes.unknown_extensions;
    }
    *out=modes;return SUNSTONE_EDID_OK;
}
