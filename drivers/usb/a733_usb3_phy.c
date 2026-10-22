/* SPDX-License-Identifier: GPL-2.0-only */
#include "a733_usb3_phy.h"
#include "a733_range.h"
#include <stdbool.h>
#define BIT(n) (UINT32_C(1)<<(n))
#define GENMASK(h,l) ((UINT32_MAX>>(31-(h)))&(UINT32_MAX<<(l)))
#define PHY_TYPE_USB3 4
#define PHY_TYPE_DP 6
#define PHY_CFG_SSC 1
#define TYPEC_ORIENTATION_REVERSE 2
#define COMBO_PHY_TYPE(mode,type) ((mode)&(1u<<(type)))
#define pr_warn(...) ((void)0)
#define pr_debug(...) ((void)0)
typedef uint32_t u32;
struct sunxi_cadence_combophy;
struct sunxi_cadence_phy { struct sunxi_cadence_combophy *combo0;struct a733_usb2_platform *platform;uintptr_t top_combo_reg;bool udp_supported; };
struct sunxi_cadence_combophy {struct sunxi_cadence_phy *sunxi_cphy;uintptr_t top_reg,phy_reg;unsigned mode,configuration,orientation;};
static void combo0_configure_usb_dp(struct sunxi_cadence_combophy *c){c->sunxi_cphy->platform->ss_failed=1;}
static u32 ss_read32(struct sunxi_cadence_phy *c,uintptr_t a)
{uint32_t v=0;if(!a733_usb3_read32(c->platform,a,&v))c->platform->ss_failed=1;return v;}
static void ss_write32(struct sunxi_cadence_phy *c,u32 v,uintptr_t a)
{if(!c->platform->ss_failed&&!a733_usb3_write32(c->platform,a,v))c->platform->ss_failed=1;}
static void ss_write16(struct sunxi_cadence_phy *c,uintptr_t a,uint16_t v)
{if(!c->platform->ss_failed&&!a733_usb3_write16(c->platform,a,v))c->platform->ss_failed=1;}
#include "ss-cadence.inc"
uint32_t a733_usb3_mask32(uintptr_t a)
{
    switch(a){case 0x06c01000:return BIT(12)|BIT(8)|BIT(5)|BIT(4)|BIT(0);
    case 0x06c01004:return BIT(24)|BIT(20)|BIT(0);
    case 0x06c01008:return 3;case 0x06c0100c:return 0x3f1;
    case 0x06c06004:return 0xff1;case 0x06c06100:case 0x06c06104:return 15;
    case 0x0709016c:return BIT(4);case 0x06a0d024:return BIT(30);
    default:return 0;}
}
int a733_usb3_address16(uintptr_t a)
{
    if(a&1||a<0x06c80000||a>=0x06ca0000)return 0;
    /* Generated from the exact selected Cadence USB function's literal indices.
     * The PMA fallback reset access is the sole additional analog index. */
    unsigned index=(unsigned)(a-0x06c80000)/2;
    switch(index){
#include "ss-cadence-addresses.inc"
    case 0xc008:return 1;default:return 0;}
}
static int update(struct a733_usb2_platform *p,uintptr_t a,uint32_t clear,uint32_t set)
{uint32_t v;return a733_usb3_read32(p,a,&v)&&a733_usb3_write32(p,a,(v&~clear)|set);}
static int wait_after(struct a733_usb2_platform *p,uint64_t delay)
{
    uint64_t now;
    if(p->ops.now(p->cookie,&now)!=1||now<p->last_now||now>=p->config.deadline||delay>=p->config.deadline-now)return 0;
    p->last_now=now;p->ss_wait=now+delay;return 1;
}
int a733_usb3_phy_poll(struct a733_usb2_platform *p)
{
    uint64_t now;uint32_t v;
    if(!p||p->ss_failed||p->ops.now(p->cookie,&now)!=1||!a733_deadline_admit(now,&p->last_now,p->config.deadline))return -1;
    if(now<p->ss_wait)return 0;
    switch(p->ss_phase){
    case 0:
        if(!update(p,0x0709016c,0,BIT(4))||!update(p,0x06c01008,3,0))return -1;
        {
            struct sunxi_cadence_phy c={.platform=p,.top_combo_reg=0x06c06000};
            struct sunxi_cadence_combophy combo={.sunxi_cphy=&c,.top_reg=0x06c01000,.phy_reg=0x06c80000,
                .mode=1u<<PHY_TYPE_USB3,.configuration=PHY_CFG_SSC,.orientation=p->attachment.polarity};c.combo0=&combo;
            combo0_usb_clk_set(&c,true);combo0_usb_param_config(&combo);
        }
        if(p->ss_failed||!update(p,0x06c01000,BIT(0),0))return -1;
        if(!wait_after(p,1000))return -1;break;
    case 1:
        if(!update(p,0x06c01000,0,BIT(0))||!update(p,0x06c06004,0,0xff1))return -1;
        if(!wait_after(p,1000))return -1;break;
    case 2:
        if(!a733_usb3_read32(p,0x06c01000,&v)||!(v&BIT(0))||
           !update(p,0x06c01000,0,BIT(4))||!update(p,0x06c0100c,0,BIT(0)))return -1;
        if(!wait_after(p,1000))return -1;break;
    case 3:
        if(!a733_usb3_read32(p,0x06c0100c,&v)||!(v&BIT(0))||
           !update(p,0x06c0100c,0,1u<<4)||!update(p,0x06c01000,0,BIT(8)))return -1;
        break;
    case 4:
        if(!a733_usb3_read32(p,0x06c01900,&v))return -1;
        if(v&1){p->ss_phase=7;return 1;}if(!wait_after(p,10000))return -1;break;
    case 5:{uint16_t a;
        if(!a733_usb3_read16(p,0x06c98010,&a)||!a733_usb3_write16(p,0x06c98010,a&~0xf00))return -1;
        if(!wait_after(p,5000))return -1;break;}
    case 6:
        if(!a733_usb3_read32(p,0x06c01900,&v)||!(v&1))return -1;
        break;
    case 7:return 1;
    default:return -1;
    }
    ++p->ss_phase;return p->ss_phase==7?1:0;
}
