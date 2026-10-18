/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
/* Called by the composed HDMI fixture; no separate leaf execution. Expected
 * fields come from manual15.2.10 and published timing arithmetic. */
void check_tcon_tv(const uint32_t *tv,const uint32_t *top,unsigned vic){
    unsigned w=vic==4?1280:1920,h=vic==4?720:1080;
    unsigned ht=vic==4?1650:2200,vt=vic==4?750:1125;
    unsigned hs=vic==4?40:44,hb=vic==4?220:148,vb=vic==4?20:36;
    assert(!(tv[0]&0x31));
    assert((tv[0x40/4]&7)==1);
    assert(!(tv[0x90/4]&0x80000000));
    assert((tv[0x98/4]&0x8001ffff)==2*vt);
    assert((tv[0x9c/4]&0x7fff7fff)==((w-1)<<16 | (h-1)));
    assert((tv[0xa0/4]&0xffff7fff)==((ht-1)<<16 | (hs+hb-1)));
    assert((tv[0xa4/4]&0x7fff)==5+vb-1);
    assert((tv[0xa8/4]&0x0fff0fff)==((hs-1)<<16 | 4));
    assert((tv[0x340/4]&7)==1);
    assert(!(top[0]&0xa));
    assert((top[0x20/4]&0x10100000)==0x10100000);
    /* Reserved high BASIC4 and legacy GCTL enable retain the initial image. */
    assert((tv[0xa4/4]&0xffff8000)==0x5a5a8000);
    assert((tv[0]&0x80000000)==0x80000000);
}
