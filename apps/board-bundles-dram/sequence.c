/* SPDX-License-Identifier: Apache-2.0 */
/* Bundle 8 sequence (docs/designs/board-bundles-next.md, row 8): pure; every
 * register access goes through the caller's pair. Port numbers and field
 * masks follow drivers/dram/a733_dram.c; port 13 is MBUS_PMU_USB_PCIE in the
 * CONFIG_ARCH_SUN60IW2 branch of the pinned sunxi-nsi.h. */
#include "sequence.h"

#define BIT(n) (UINT32_C(1)<<(n))
static const uint8_t ports[A733_DRAM_PORTS]={2,14,15};
static const uint8_t offsets[3]={0x10,0x14,0x18};
static const uint8_t masks[3]={3,15,1};

uintptr_t bb8_field(unsigned i)
{return A733_NSI_PORT(ports[i/3])+offsets[i%3];}
uint32_t bb8_mask(unsigned i)
{return masks[i%3];}
uintptr_t bb8_usb_pcie(unsigned i)
{return A733_NSI_PORT(BB8_USB_PCIE_PORT)+offsets[i];}
static int rd(const struct a733_dram_io *io,uintptr_t a,uint32_t *v)
{return io->read(io->context,a,v)==1;}

int bb8_gates(const struct a733_dram_io *io,struct bb8_record *r)
{
    *r=(struct bb8_record){0};
    if(!rd(io,BB8_R_CCU_PPU,&r->ppu_clock))return -1;
    /* The PPU is read only behind its open bus clock, as in bundle 5. */
    if(r->ppu_clock&1){
        if(!rd(io,BB8_DOMAIN1_STATUS,&r->domain1_status))return -1;
        r->domain1_read=1;
    }
    if(!rd(io,A733_NSI_CLOCK,&r->nsi_clock) || !rd(io,A733_NSI_CONFIG,&r->nsi_config) ||
       !rd(io,A733_MBUS_CLOCK,&r->mbus_clock))return -1;
    r->skipped=!(r->ppu_clock&1) ? BB8_SKIP_PPU_CLOCK : (r->domain1_status&15)!=8 ? BB8_SKIP_DOMAIN1 :
        (r->nsi_clock&(BIT(31)|BIT(30)))!=(BIT(31)|BIT(30)) ? BB8_SKIP_NSI_CLOCK :
        (r->nsi_config&(BIT(16)|BIT(0)))!=(BIT(16)|BIT(0)) ? BB8_SKIP_NSI_CONFIG : BB8_READY;
    return r->skipped==BB8_READY;
}

enum bb8_result bb8_nsi(const struct a733_dram_io *io,struct bb8_record *r)
{
    unsigned zero=1;
    for(unsigned i=0;i<BB8_FIELDS;++i){
        if(!rd(io,bb8_field(i),&r->inherited[i]))return BB8_ACCESS_FAILED;
        zero&=!(r->inherited[i]&bb8_mask(i));
    }
    /* A Secure-only NSI can answer Non-secure reads with zeros; the write
     * readback separates that case from inherited zero fields. */
    r->possibly_protected=zero;
    for(unsigned i=0;i<3;++i)if(!rd(io,bb8_usb_pcie(i),&r->usb_pcie_before[i]))return BB8_ACCESS_FAILED;
    if(!a733_dram_priority(io,&r->block))
        return r->block.failed==A733_DRAM_CCU_FAILED ? BB8_ACCESS_FAILED : r->block.failed ? BB8_MISMATCH :
            BB8_GATE_CHANGED;
    for(unsigned i=0;i<3;++i){
        if(!rd(io,bb8_usb_pcie(i),&r->usb_pcie_after[i]))return BB8_ACCESS_FAILED;
        if((r->usb_pcie_after[i]^r->usb_pcie_before[i])&masks[i])return BB8_USB_PCIE_CHANGED;
    }
    return BB8_WRITTEN;
}
