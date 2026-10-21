/* SPDX-License-Identifier: GPL-2.0-only */
/* VIPP parameter values and enable order adapted from the A733 camera drivers.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2007-2021 Allwinnertech Co., Ltd.
 * Authors: Zheng ZeQun <zequnzheng@allwinnertech.com>
 */
#ifndef SUNSTONE_A733_CAMERA_COMMON_H
#define SUNSTONE_A733_CAMERA_COMMON_H
#include <stddef.h>
#include <stdint.h>

/* Helpers shared by the camera controllers. Each controller keeps its own
 * register access granularity and fault state; these only remove copies. */
static inline int a733_camera_overlap(const void *a,size_t an,const void *b,size_t bn)
{
    uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an>UINTPTR_MAX-x || bn>UINTPTR_MAX-y || (x<y+bn && y<x+an);
}
/* Store a fresh monotonic time before the deadline. 0 means the clock failed,
 * went backward or reached the deadline; the caller faults. */
static inline int a733_camera_clock(int (*now_us)(void *,uint64_t *),void *context,
                                    uint64_t *now,uint64_t deadline)
{
    uint64_t n;
    if(now_us(context,&n)!=1 || n<*now || n>=deadline)return 0;
    *now=n;return 1;
}
/* Adopt the token an admission callback published and check its result.
 * A held token must not change, and success needs a nonzero token. Returns
 * the result 0/1, or -1 for a fault; a first token is kept even then. */
static inline int a733_camera_adopt(uint64_t *held,uint64_t token,int r)
{
    if(*held && token!=*held)return -1;
    if(!*held)*held=token;
    return r<0 || r>1 || (r && !*held)?-1:r;
}

/* Complete 1 KiB VIPP parameter image for unity 640x480 YUV422 output with
 * no conversion or overlays, including reserved bytes. The separate Radxa A733
 * source has no SC_CFG1 precision field, so offset 0x20c stays zero. The
 * source unity ratio calculation returns weight shift 1. */
static inline void a733_vipp_unity_parameters(uint8_t p[1024])
{
    static const struct { uint16_t off;uint32_t value; } words[]={
        {0x200,1},{0x204,256u|(256u<<14)|(1u<<27)},{0x208,640u|(480u<<16)},
        {0x220,0xd0},{0x234,640u|(480u<<16)}
    };
    for(unsigned i=0;i<1024;i++)p[i]=0;
    for(unsigned i=0;i<sizeof(words)/sizeof(words[0]);i++)
        for(unsigned b=0;b<4;b++)p[words[i].off+b]=(uint8_t)(words[i].value>>(8*b));
}
/* VIPP0 enable order. UPDATE is a masked read-modify-write with readback.
 * W1C writes the value and requires zero readback. ADDRESS installs the
 * caller's parameter IOVA >> 2. READY is an UPDATE of the capture register
 * whose PARA_READY bit (bit 2) can clear once the parameters load, so its
 * readback checks only CAP (bit 0) and BYPASS (bit 4). */
enum a733_vipp_kind { A733_VIPP_UPDATE=0,A733_VIPP_ADDRESS=2,A733_VIPP_W1C=3,A733_VIPP_READY=4 };
#define A733_VIPP_READY_VERIFY 0x11u
struct a733_vipp_step { uint16_t off;uint8_t kind;uint32_t mask,value; };
static const struct a733_vipp_step a733_vipp_enable[]={
    {0x000,A733_VIPP_UPDATE,0x13,1}, /* Online, clock on, capture off. */
    {0x030,A733_VIPP_W1C,UINT32_MAX,0xffffff03},
    {0x040,A733_VIPP_UPDATE,0x15,0},
    {0x050,A733_VIPP_ADDRESS,UINT32_MAX,0},
    {0x000,A733_VIPP_UPDATE,2,2},
    {0x040,A733_VIPP_READY,0x15,4},
    {0x040,A733_VIPP_READY,1,1}
};
#define A733_VIPP_ENABLE_STEPS (sizeof(a733_vipp_enable)/sizeof(a733_vipp_enable[0]))
#endif
