/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Port field writes adapted from nsi_port_setmode(), nsi_port_setpri() and
 * nsi_port_setio() in sunxi-nsi.c; the table is the nsi0 node of
 * sun60iw2p1.dtsi.
 * Port numbers: the CONFIG_ARCH_SUN60IW2 branch of enum nsi_pmu in
 * sunxi-nsi.h gives DE 2, IOMMU0 14 and IOMMU1 15, the ids of the nsi0
 * child nodes. That node sets sub_node_id_mapping, so sunxi-nsi.c uses each
 * id as the register index; its subtract-one mapping needs
 * NSI_HARDCODED_PORT_MAPPING, which sunxi-nsi.h defines only for sun55iw3.
 * The pinned U-Boot drivers/nsi/sunxi_nsi.c addresses the same words at
 * 0x02020000 + 0x200 * id.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
#include "a733_dram.h"
#if SUNSTONE_DRAM_PRIORITY!=1 && SUNSTONE_DRAM_PRIORITY!=2
#error "SUNSTONE_DRAM_PRIORITY selects 1 (reads only) or 2 (table)"
#endif
#define BIT(n) (UINT32_C(1)<<(n))
/* Per port: mode bits 1-0 of +0x10 (0), both priority subfields of +0x14
 * ((p << 2) | p) and select bit 0 of +0x18 (1). shift places the field in
 * the evidence: mode bits 1-0, priority bits 5-2, select bit 6. One flat
 * entry per field keeps the loop to loads and compares. */
#define PORT_FIELDS(n,p) {A733_NSI_PORT(n)+0x10,3,0,0},{A733_NSI_PORT(n)+0x14,15,(p)<<2|(p),2},\
    {A733_NSI_PORT(n)+0x18,1,1,6}
static const struct { uint32_t address; uint8_t mask,want,shift; } fields[3*A733_DRAM_PORTS]={
    PORT_FIELDS(2,2),PORT_FIELDS(14,3),PORT_FIELDS(15,3)};
int a733_dram_address(uintptr_t a)
{
    if(a==A733_NSI_CLOCK||a==A733_NSI_CONFIG)return 1;
    for(unsigned i=0;i<3*A733_DRAM_PORTS;++i)if(a==fields[i].address)return 1;
    return 0;
}
int a733_dram_priority(const struct a733_dram_io *io,struct a733_dram_priority_evidence *e)
{
    *e=(struct a733_dram_priority_evidence){0};
    if(io->read(io->context,A733_NSI_CLOCK,&e->nsi_clock)!=1||
       io->read(io->context,A733_NSI_CONFIG,&e->nsi_config)!=1)return 0;
    /* A set bit opens a gate or deasserts a reset. The media clock compares
     * only gate bit 31 of +0x580; this adds its reset and the +0x584 word. */
    if((e->nsi_clock&(BIT(31)|BIT(30)))!=(BIT(31)|BIT(30))||
       (e->nsi_config&(BIT(16)|BIT(0)))!=(BIT(16)|BIT(0)))return 0;
    for(unsigned i=0;i<3*A733_DRAM_PORTS;++i){
        uint32_t a=fields[i].address,mask=fields[i].mask,want=fields[i].want,word,actual;
        unsigned port=i/3;
        e->failed=(uint16_t)(i+1);
        if(io->read(io->context,a,&word)!=1)return 0;
        e->inherited[port]|=(uint16_t)((word&mask)<<fields[i].shift);
        if(SUNSTONE_DRAM_PRIORITY==2&&(word&mask)!=want){
            if(io->write(io->context,a,(word&~mask)|want)!=1||
               io->read(io->context,a,&actual)!=1||(actual&mask)!=want)return 0;
            word=actual;
        }
        e->final[port]|=(uint16_t)((word&mask)<<fields[i].shift);
    }
    e->failed=0;return 1;
}
void a733_dram_block(const struct a733_dram_priority_evidence *e,int hook_result,struct a733_dram_block *b)
{
    *b=(struct a733_dram_block){
        .result=(uint16_t)(A733_DRAM_HOOK_BUILT|(SUNSTONE_DRAM_PRIORITY==2?A733_DRAM_TABLE_IMAGE:0)|
            (hook_result==1?A733_DRAM_HOOK_PASSED:0)),
        .failed=e->failed,.nsi_clock=e->nsi_clock,.nsi_config=e->nsi_config};
    for(unsigned i=0;i<A733_DRAM_PORTS;++i){b->inherited[i]=e->inherited[i];b->final[i]=e->final[i];}
}
