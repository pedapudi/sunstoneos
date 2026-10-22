/* SPDX-License-Identifier: GPL-2.0-only */
/* Adapted from the pinned A733 Linux camera implementation.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2007-2021 Allwinnertech Co., Ltd.
 * Authors: Zheng ZeQun <zequnzheng@allwinnertech.com>
 * See isp0-sources.json for source identity and RAM/MMIO boundaries. */
#include "a733_isp0.h"
#include "a733_camera_common.h"
#include "isp600_default.h"
#include <string.h>
#define CCU UINT32_C(0x05800000)
#define TOP UINT32_C(0x05800800)
#define ISP UINT32_C(0x05900000)
#define VIPP UINT32_C(0x05910000)
static uint32_t get(const uint8_t *p)
{return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put(uint8_t *p,uint32_t v)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void patch(uint8_t *p,unsigned off,uint32_t mask,uint32_t value)
{put(p+off,(get(p+off)&~mask)|(value&mask));}
int a733_isp0_parameters(void *load,size_t ln,void *save_load,size_t sn,void *vipp,size_t vn)
{
 if(!load||!save_load||!vipp||ln<A733_ISP0_LOAD_BYTES||sn<A733_ISP0_SAVE_LOAD_BYTES||vn<1024)return 0;
 uint8_t *p=load,*s=save_load,*v=vipp;
 memset(p,0,A733_ISP0_LOAD_BYTES);memset(s,0,A733_ISP0_SAVE_LOAD_BYTES);memset(v,0,1024);
 for(unsigned i=0;i<1024;i++)put(p+4*i,a733_isp600_default[i]);
 /* bsp_isp_map_load_dram_addr maps load registers at image+0x100.
  * bsp_isp_set_input_fmt also updates the separate save-load random seed. */
 patch(p,0x100,7,4);patch(s,0x30,0x70000000,4u<<28);
 patch(p,0x104,0x77,0x53); /* Last blank5, speed3. */
 patch(p,0x120,0x3fff3fff,640u|(480u<<16));
 patch(p,0x124,0x3fff3fff,640u|(480u<<16));
 patch(p,0x128,0x1fff1fff,0);
 patch(p,0x198,0x1f0,12u<<4); /* Channel0 input_bit=RAW_12. */
 /* Source module0/module1 disable-all setters. No 3A, WB, lens, gamma,
  * CEM, temporal history or auxiliary image addressing is enabled. */
 put(p+0x130,0);put(p+0x134,0);
 a733_vipp_unity_parameters(v,640,480);
 return 1;
}
struct step {uintptr_t address;uint32_t mask,value;unsigned kind;};
/* kind0 checked masked update; kind1 W1C and verify zero; kind2 cold check;
 * kind3 VIPP para-ready update. ISP parameter RAM is never replayed to MMIO.
 * Only owned DMA addresses are installed through the source-defined AHB
 * address registers. The shared VIPP0 enable order follows this table. */
static const struct step prepare[]={
 {CCU+4,UINT32_MAX,0,2},{TOP,0x000ff00d,0,2},
 {TOP+4,0x00ff0011,0,2},{ISP,UINT32_MAX,0,2},
 {ISP+0x20,UINT32_MAX,0,2},{ISP+0x50,UINT32_MAX,0,2},{ISP+0x80,UINT32_MAX,0,2},{ISP+0xb0,UINT32_MAX,0,2},
 {ISP+0x28,UINT32_MAX,0,2},{ISP+0x58,UINT32_MAX,0,2},
 {ISP+0x88,UINT32_MAX,0,2},{ISP+0xb8,UINT32_MAX,0,2},
 {VIPP,0x53,0,2},{VIPP+0x20,UINT32_MAX,0,2},{VIPP+0x24,UINT32_MAX,0,2},{VIPP+0x40,0x15,0,2},
 {VIPP+0x80,0x15,0,2},{VIPP+0xc0,0x15,0,2},{VIPP+0x100,0x15,0,2},
 {CCU,0x80000003,3,0}, /* Existing CCU clock/post mode setters. */
 {CCU+4,0x101,0x101,0}, /* Source pattern pipeline still clocks COMBO0/parser0. */
 {CCU+8,0x00111311,0x00011011,0}, /* ISP0 + bridge, channel0 both ways; resets deasserted. */
 {CCU+0xc,0x00010101,0x00010101,0}, /* POST0,VIPP0,BK0 clocks. */
 {TOP,0x0011110d,0x00011009,0}, /* TOP, ISP bridge and channel0; SRAM awake. */
 {TOP+0x30,15,0,0},{TOP+0xa0,15,0,0}, /* ISP0 CH0 -> VIPP0. */
 {TOP+8,0x073f00ff,0x062c0008,0}, /* COMBO, RAW12, ISPFE Bayer mode12, delay8. */
 {TOP+0x20,UINT32_MAX,A733_ISP0_INPUT_BYTES,0},
 {TOP+0x24,UINT32_MAX,A733_ISP0_INPUT_IOVA>>2,0},
 {TOP+0x28,0x1fff1fff,640u|(480u<<16),0},
 {ISP,7,5,0}, /* enabled, online, top capture. */
 {ISP+0x2c,UINT32_MAX,UINT32_MAX,1},
 {ISP+0x24,UINT32_MAX,A733_ISP0_LOAD_IOVA>>2,0},
 {ISP+0x3c,UINT32_MAX,A733_ISP0_SAVE_IOVA>>2,0},
 {ISP+0x40,UINT32_MAX,A733_ISP0_SAVE_LOAD_IOVA>>2,0},
 {ISP+0x20,0x601fe703,0x60008203,0} /* gamma/CEM update, READY,CAP, feedback. */
};
#define PREPARE (sizeof(prepare)/sizeof(*prepare))
static struct step prepared(unsigned i)
{
 if(i<PREPARE)return prepare[i];
 const struct a733_vipp_step *e=&a733_vipp_enable[i-PREPARE];
 if(e->kind==A733_VIPP_ADDRESS)return (struct step){VIPP+e->off,UINT32_MAX,A733_ISP0_VIPP_IOVA>>2,0};
 return (struct step){VIPP+e->off,e->mask,e->value,e->kind==A733_VIPP_W1C?1u:e->kind==A733_VIPP_READY?3u:0u};
}
static int fail(struct a733_isp0 *s){s->fault=1;return -1;}
static int timely(struct a733_isp0 *s)
{return a733_camera_clock(s->ops.now,s->context,&s->last_now,s->deadline)?1:fail(s);}
static int read32(struct a733_isp0 *s,uintptr_t a,uint32_t *v)
{return timely(s)==1&&s->ops.read(s->context,a,v)==1&&timely(s)==1?1:fail(s);}
static int write32(struct a733_isp0 *s,uintptr_t a,uint32_t v)
{return timely(s)==1&&s->ops.write(s->context,a,v)==1&&timely(s)==1?1:fail(s);}
int a733_isp0_init(struct a733_isp0 *s,const struct a733_isp0_ops *ops,void *ctx,uint64_t deadline)
{
 if(!s||s->initialized||!ops||!ops->now||!ops->read||!ops->write||!deadline)return 0;
 *s=(struct a733_isp0){.ops=*ops,.context=ctx,.deadline=deadline,.initialized=1};return 1;
}
int a733_isp0_poll(struct a733_isp0 *s)
{
 if(!s||!s->initialized||s->fault)return -1;if(timely(s)!=1)return -1;if(s->armed)return 1;
 const struct step step=prepared(s->index),*p=&step;uint32_t v;
 if(read32(s,p->address,&v)!=1)return -1;
 if(p->kind==2){if((v&p->mask)!=p->value)return fail(s);}
 else{
  uint32_t value=p->kind==1?p->value:(v&~p->mask)|p->value;
  if(write32(s,p->address,value)!=1||read32(s,p->address,&v)!=1)return -1;
  /* PARA_READY can clear after loading; capture/bypass remain checked. */
  uint32_t mask=p->address==ISP+0x20?p->mask&~2u:p->kind==3?A733_VIPP_READY_VERIFY:p->mask;
  if(p->kind==1?v!=0:(v&mask)!=(value&mask))return fail(s);
 }
 if(++s->index==PREPARE+A733_VIPP_ENABLE_STEPS)s->armed=1;
 return s->armed?1:0;
}
int a733_isp0_trigger(struct a733_isp0 *s)
{
 uint32_t v;if(!s||!s->armed||s->started||s->fault||read32(s,TOP+4,&v)!=1)return -1;
 if(v&0x00ff0011)return fail(s);
 /* Source generation_en writes cycle0, GEN1 then START1. START auto-clears. */
 if(write32(s,TOP+4,(v&~0x00ff0011u)|1)!=1||
    write32(s,TOP+4,(v&~0x00ff0011u)|0x11)!=1)return -1;
 s->started=1;return 1;
}
int a733_isp0_complete(struct a733_isp0 *s)
{
 if(!s||!s->started||s->fault)return -1;
 uint32_t i,v,p;if(read32(s,ISP+0x2c,&i)!=1||read32(s,VIPP+0x30,&v)!=1||read32(s,TOP+4,&p)!=1)return -1;
 if((i&~0x1fu)||(v&~0x100u)||(p&0x00ff0000)||!(p&1))return fail(s);
 if((i&0xc)!=0xc||v!=0x100||(p&0x10))return 0;
 s->isp_status=i;s->vipp_status=v;s->pattern_status=p;return 1;
}
