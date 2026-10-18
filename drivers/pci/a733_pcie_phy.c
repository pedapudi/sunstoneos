// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Allwinner USB3.0/PCIE/DisplayPort Combo Phy driver
 * Copyright (C) 2024 Allwinner Electronics Co., Ltd.
 * Internal 100 MHz register sequence adapted from sunxi-cadence-combophy.c.
 */
#include "a733_pcie.h"
struct step { uintptr_t address; unsigned width; uint32_t mask,value; };
static const struct step sequence[]={
    {0x06c02004, 4, 0xffffffffu, 0x01100001u},
    {0x06ca0044, 2, 0x0000ffffu, 0x0000001au},
    {0x06ca0054, 2, 0x0000ffffu, 0x00000034u},
    {0x06ca0058, 2, 0x0000ffffu, 0x000000dau},
    {0x06ca0064, 2, 0x0000ffffu, 0x00000034u},
    {0x06ca0068, 2, 0x0000ffffu, 0x000000dau},
    {0x06ca00c8, 2, 0x0000ffffu, 0x00000082u},
    {0x06ca00ca, 2, 0x0000ffffu, 0x00000082u},
    {0x06ca00e8, 2, 0x0000ffffu, 0x0000001au},
    {0x06ca0208, 2, 0x0000ffffu, 0x00000020u},
    {0x06ca020a, 2, 0x0000ffffu, 0x00000007u},
    {0x06ca0218, 2, 0x0000ffffu, 0x00000020u},
    {0x06ca021a, 2, 0x0000ffffu, 0x00000007u},
    {0x06ca0228, 2, 0x0000ffffu, 0x0000030cu},
    {0x06ca022a, 2, 0x0000ffffu, 0x00000007u},
    {0x06ca0248, 2, 0x0000ffffu, 0x00000007u},
    {0x06ca024a, 2, 0x0000ffffu, 0x00000003u},
    {0x06ca024c, 2, 0x0000ffffu, 0x0000000fu},
    {0x06ca0250, 2, 0x0000ffffu, 0x00000132u},
    {0x06ca8180, 2, 0x0000ffffu, 0x00000208u},
    {0x06ca8184, 2, 0x0000ffffu, 0x0000009cu},
    {0x06cb0088, 2, 0x0000ffffu, 0x0000001au},
    {0x06cb008a, 2, 0x0000ffffu, 0x00000082u},
    {0x06cb0098, 2, 0x0000ffffu, 0x0000001au},
    {0x06cb009a, 2, 0x0000ffffu, 0x00000082u},
    {0x06ca8246, 2, 0x0000ffffu, 0x00000a28u},
    {0x06c02100, 4, 0x00000030u, 0x00000000u},
    {0x06ca0128, 2, 0x0000ffffu, 0x00000004u},
    {0x06ca0148, 2, 0x0000ffffu, 0x00000004u},
    {0x06ca01a8, 2, 0x0000ffffu, 0x00000004u},
    {0x06ca0348, 2, 0x0000ffffu, 0x00000509u},
    {0x06ca0368, 2, 0x0000ffffu, 0x00000509u},
    {0x06ca0388, 2, 0x0000ffffu, 0x00000509u},
    {0x06ca034a, 2, 0x0000ffffu, 0x00000f00u},
    {0x06ca036a, 2, 0x0000ffffu, 0x00000f00u},
    {0x06ca038a, 2, 0x0000ffffu, 0x00000f00u},
    {0x06ca034c, 2, 0x0000ffffu, 0x00000f08u},
    {0x06ca036c, 2, 0x0000ffffu, 0x00000f08u},
    {0x06ca038c, 2, 0x0000ffffu, 0x00000f08u},
    {0x06ca0120, 2, 0x0000ffffu, 0x00000180u},
    {0x06ca0140, 2, 0x0000ffffu, 0x00000133u},
    {0x06ca01a0, 2, 0x0000ffffu, 0x00000133u},
    {0x06ca0122, 2, 0x0000ffffu, 0x00009d8au},
    {0x06ca0142, 2, 0x0000ffffu, 0x0000b13bu},
    {0x06ca01a2, 2, 0x0000ffffu, 0x0000b13bu},
    {0x06ca0124, 2, 0x0000ffffu, 0x00000002u},
    {0x06ca0144, 2, 0x0000ffffu, 0x00000002u},
    {0x06ca01a4, 2, 0x0000ffffu, 0x00000002u},
    {0x06ca0126, 2, 0x0000ffffu, 0x00000102u},
    {0x06ca0146, 2, 0x0000ffffu, 0x000000ceu},
    {0x06ca01a6, 2, 0x0000ffffu, 0x000000ceu},
    {0x06ca0340, 2, 0x0000ffffu, 0x00000022u},
    {0x06ca0360, 2, 0x0000ffffu, 0x00000022u},
    {0x06ca0380, 2, 0x0000ffffu, 0x00000022u},
    {0x06ca0130, 2, 0x0000ffffu, 0x00000001u},
    {0x06ca0150, 2, 0x0000ffffu, 0x00000001u},
    {0x06ca01b0, 2, 0x0000ffffu, 0x00000001u},
    {0x06ca0132, 2, 0x0000ffffu, 0x0000045fu},
    {0x06ca0152, 2, 0x0000ffffu, 0x000002f0u},
    {0x06ca01b2, 2, 0x0000ffffu, 0x00000399u},
    {0x06ca0134, 2, 0x0000ffffu, 0x0000006bu},
    {0x06ca0154, 2, 0x0000ffffu, 0x00000068u},
    {0x06ca01b4, 2, 0x0000ffffu, 0x00000068u},
    {0x06ca0136, 2, 0x0000ffffu, 0x00000004u},
    {0x06ca0156, 2, 0x0000ffffu, 0x00000004u},
    {0x06ca01b6, 2, 0x0000ffffu, 0x00000004u},
    {0x06ca0108, 2, 0x0000ffffu, 0x00000104u},
    {0x06ca0188, 2, 0x0000ffffu, 0x00000104u},
    {0x06ca010a, 2, 0x0000ffffu, 0x00000005u},
    {0x06ca018a, 2, 0x0000ffffu, 0x00000005u},
    {0x06ca010c, 2, 0x0000ffffu, 0x00000337u},
    {0x06ca018c, 2, 0x0000ffffu, 0x00000337u},
    {0x06ca0110, 2, 0x0000ffffu, 0x00003dbeu},
    {0x06ca0190, 2, 0x0000ffffu, 0x00003dbeu},
    {0x06ca0104, 2, 0x0000ffffu, 0x00000003u},
    {0x06ca0184, 2, 0x0000ffffu, 0x00000003u},
    {0x06ca0138, 2, 0x0000ffffu, 0x00000014u},
    {0x06ca01b8, 2, 0x0000ffffu, 0x00000014u},
    {0x06ca013c, 2, 0x0000ffffu, 0x00000192u},
    {0x06ca01bc, 2, 0x0000ffffu, 0x00000192u},
    {0x06ca013e, 2, 0x0000ffffu, 0x00000006u},
    {0x06ca01be, 2, 0x0000ffffu, 0x00000006u},
    {0x06cb03c0, 2, 0x0000ffffu, 0x00000000u},
    {0x06cb02e2, 2, 0x0000ffffu, 0x00000019u},
    {0x06cb02e4, 2, 0x0000ffffu, 0x00000019u},
    {0x06cb03fe, 2, 0x0000ffffu, 0x00000001u},
    {0x06ca0098, 4, 0x00000003u, 0x00000002u},
    {0x06ca00a8, 4, 0x00000003u, 0x00000002u},
    {0x06ca00d8, 4, 0x00000003u, 0x00000002u},
    {0x06c02000, 4, 0x00000001u, 0x00000001u},
    {0x06c02100, 4, 0x00000001u, 0x00000001u},
};
static const struct step finish[]={
    {0x06ca00a0,2,0xffff,0x270},
    {0x06ca0098,2,0x10,0x10},
    {0x06cb8000,2,1,1},
    {0x06c02004,4,0x10000000,0x10000000}
};
int a733_pcie_phy_address(uintptr_t a,unsigned width,unsigned writing)
{
    if(!writing && a==0x06c02900 && width==4)return 1;
    for(unsigned i=0;i<sizeof(sequence)/sizeof(sequence[0]);++i)
        if(sequence[i].address==a&&sequence[i].width==width)return 1;
    for(unsigned i=0;i<sizeof(finish)/sizeof(finish[0]);++i)
        if(finish[i].address==a&&finish[i].width==width)return 1;
    return 0;
}
int a733_pcie_phy_step(struct a733_pcie *p)
{
    const unsigned count=sizeof(sequence)/sizeof(sequence[0]);
    const struct step *s;
    uint32_t old=0;
    if(p->phy_index==count) {
        if(p->last_now<p->wait_until)return 0;
        if(!a733_pcie_read(p,0x06c02900,4,&old))return -1;
        if(!(old&1)) {p->wait_until=p->last_now+100;return 0;}
        ++p->phy_index;return 0;
    }
    if(p->phy_index<count)s=&sequence[p->phy_index];
    else if(p->phy_index-count-1<sizeof(finish)/sizeof(finish[0]))s=&finish[p->phy_index-count-1];
    else return 1;
    uint32_t full=s->width==2?0xffffu:UINT32_MAX;
    if(s->mask!=full&&!a733_pcie_read(p,s->address,s->width,&old))return -1;
    if(!a733_pcie_write(p,s->address,s->width,(old&~s->mask)|s->value))return -1;
    ++p->phy_index;
    if(p->phy_index==count)p->wait_until=p->last_now+100;
    return 0;
}
