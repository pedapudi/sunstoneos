/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Copyright (c) 2007-2019 Allwinnertech Co., Ltd.
 * Author: zhengxiaobin <zhengxiaobin@allwinnertech.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/* Bounded FreeRTOS RCQ adaptation; source pins: apps/g2d/sources.json. */
#include "a733_g2d.h"
#include "g2d_mixer_type.h"
#include <string.h>
/* RGB scaling uses the pinned g2d_scal.c coefficient banks unchanged: Lanczos
 * horizontal phases selected by ratio and linear vertical phases. */
static const uint32_t linearcoefftab32[32] = {
	0x00004000, 0x00023e00, 0x00043c00, 0x00063a00, 0x00083800,
	0x000a3600, 0x000c3400, 0x000e3200, 0x00103000, 0x00122e00,
	0x00142c00, 0x00162a00, 0x00182800, 0x001a2600, 0x001c2400,
	0x001e2200, 0x00202000, 0x00221e00, 0x00241c00, 0x00261a00,
	0x00281800, 0x002a1600, 0x002c1400, 0x002e1200, 0x00301000,
	0x00320e00, 0x00340c00, 0x00360a00, 0x00380800, 0x003a0600,
	0x003c0400, 0x003e0200, };

static const uint32_t lan2coefftab32_full[512] = {
	0x00004000, 0x000140ff, 0x00033ffe, 0x00043ffd, 0x00063efc, 0xff083dfc,
	0x000a3bfb, 0xff0d39fb, 0xff0f37fb, 0xff1136fa, 0xfe1433fb,
	0xfe1631fb, 0xfd192ffb, 0xfd1c2cfb, 0xfd1f29fb, 0xfc2127fc,
	0xfc2424fc, 0xfc2721fc, 0xfb291ffd, 0xfb2c1cfd, 0xfb2f19fd,
	0xfb3116fe, 0xfb3314fe, 0xfa3611ff, 0xfb370fff, 0xfb390dff,
	0xfb3b0a00, 0xfc3d08ff, 0xfc3e0600, 0xfd3f0400, 0xfe3f0300,
	0xff400100,
	/* counter = 1 */
	0x00004000, 0x000140ff, 0x00033ffe, 0x00043ffd, 0x00063efc,
	0xff083dfc, 0x000a3bfb, 0xff0d39fb, 0xff0f37fb, 0xff1136fa,
	0xfe1433fb, 0xfe1631fb, 0xfd192ffb, 0xfd1c2cfb, 0xfd1f29fb,
	0xfc2127fc, 0xfc2424fc, 0xfc2721fc, 0xfb291ffd, 0xfb2c1cfd,
	0xfb2f19fd, 0xfb3116fe, 0xfb3314fe, 0xfa3611ff, 0xfb370fff,
	0xfb390dff, 0xfb3b0a00, 0xfc3d08ff, 0xfc3e0600, 0xfd3f0400,
	0xfe3f0300, 0xff400100,
	/* counter = 2 */
	0xff053804, 0xff063803, 0xff083801, 0xff093701, 0xff0a3700,
	0xff0c3500, 0xff0e34ff, 0xff1033fe, 0xff1232fd, 0xfe1431fd,
	0xfe162ffd, 0xfe182dfd, 0xfd1b2cfc, 0xfd1d2afc, 0xfd1f28fc,
	0xfd2126fc, 0xfd2323fd, 0xfc2621fd, 0xfc281ffd, 0xfc2a1dfd,
	0xfc2c1bfd, 0xfd2d18fe, 0xfd2f16fe, 0xfd3114fe, 0xfd3212ff,
	0xfe3310ff, 0xff340eff, 0x00350cff, 0x00360a00, 0x01360900,
	0x02370700, 0x03370600,
	/* counter = 3 */
	0xff083207, 0xff093206, 0xff0a3205, 0xff0c3203, 0xff0d3103,
	0xff0e3102, 0xfe113001, 0xfe132f00, 0xfe142e00, 0xfe162dff,
	0xfe182bff, 0xfe192aff, 0xfe1b29fe, 0xfe1d27fe, 0xfe1f25fe,
	0xfd2124fe, 0xfe2222fe, 0xfe2421fd, 0xfe251ffe, 0xfe271dfe,
	0xfe291bfe, 0xff2a19fe, 0xff2b18fe, 0xff2d16fe, 0x002e14fe,
	0x002f12ff, 0x013010ff, 0x02300fff, 0x03310dff, 0x04310cff,
	0x05310a00, 0x06310900,
	/* counter = 4 */
	0xff0a2e09, 0xff0b2e08, 0xff0c2e07, 0xff0e2d06, 0xff0f2d05,
	0xff102d04, 0xff122c03, 0xfe142c02, 0xfe152b02, 0xfe172a01,
	0xfe182901, 0xfe1a2800, 0xfe1b2700, 0xfe1d2500, 0xff1e24ff,
	0xfe2023ff, 0xff2121ff, 0xff2320fe, 0xff241eff, 0x00251dfe,
	0x00261bff, 0x00281afe, 0x012818ff, 0x012a16ff, 0x022a15ff,
	0x032b13ff, 0x032c12ff, 0x052c10ff, 0x052d0fff, 0x062d0d00,
	0x072d0c00, 0x082d0b00,
	/* counter = 5 */
	0xff0c2a0b, 0xff0d2a0a, 0xff0e2a09, 0xff0f2a08, 0xff102a07,
	0xff112a06, 0xff132905, 0xff142904, 0xff162803, 0xff172703,
	0xff182702, 0xff1a2601, 0xff1b2501, 0xff1c2401, 0xff1e2300,
	0xff1f2200, 0x00202000, 0x00211f00, 0x01221d00, 0x01231c00,
	0x01251bff, 0x02251aff, 0x032618ff, 0x032717ff, 0x042815ff,
	0x052814ff, 0x052913ff, 0x06291100, 0x072a10ff, 0x082a0e00,
	0x092a0d00, 0x0a2a0c00,
	/* counter = 6 */
	0xff0d280c, 0xff0e280b, 0xff0f280a, 0xff102809, 0xff112808,
	0xff122708, 0xff142706, 0xff152705, 0xff162605, 0xff172604,
	0xff192503, 0xff1a2403, 0x001b2302, 0x001c2202, 0x001d2201,
	0x001e2101, 0x011f1f01, 0x01211e00, 0x01221d00, 0x02221c00,
	0x02231b00, 0x03241900, 0x04241800, 0x04251700, 0x052616ff,
	0x06261400, 0x072713ff, 0x08271100, 0x08271100, 0x09271000,
	0x0a280e00, 0x0b280d00,
	/* counter = 7 */
	0xff0e260d, 0xff0f260c, 0xff10260b, 0xff11260a, 0xff122609,
	0xff132608, 0xff142508, 0xff152507, 0x00152506, 0x00172405,
	0x00182305, 0x00192304, 0x001b2203, 0x001c2103, 0x011d2002,
	0x011d2002, 0x011f1f01, 0x021f1e01, 0x02201d01, 0x03211c00,
	0x03221b00, 0x04221a00, 0x04231801, 0x05241700, 0x06241600,
	0x07241500, 0x08251300, 0x09251200, 0x09261100, 0x0a261000,
	0x0b260f00, 0x0c260e00,
	/* counter = 8 */
	0xff0e250e, 0xff0f250d, 0xff10250c, 0xff11250b, 0x0011250a,
	0x00132409, 0x00142408, 0x00152407, 0x00162307, 0x00172306,
	0x00182206, 0x00192205, 0x011a2104, 0x011b2004, 0x011c2003,
	0x021c1f03, 0x021e1e02, 0x031e1d02, 0x03201c01, 0x04201b01,
	0x04211a01, 0x05221900, 0x05221801, 0x06231700, 0x07231600,
	0x07241500, 0x08241400, 0x09241300, 0x0a241200, 0x0b241100,
	0x0c241000, 0x0d240f00,
	/* counter = 9 */
	0x000e240e, 0x000f240d, 0x0010240c, 0x0011240b, 0x0013230a,
	0x0013230a, 0x00142309, 0x00152308, 0x00162208, 0x00172207,
	0x01182106, 0x01192105, 0x011a2005, 0x021b1f04, 0x021b1f04,
	0x021d1e03, 0x031d1d03, 0x031e1d02, 0x041e1c02, 0x041f1b02,
	0x05201a01, 0x05211901, 0x06211801, 0x07221700, 0x07221601,
	0x08231500, 0x09231400, 0x0a231300, 0x0a231300, 0x0b231200,
	0x0c231100, 0x0d231000,
	/* counter = 10 */
	0x000f220f, 0x0010220e, 0x0011220d, 0x0012220c, 0x0013220b,
	0x0013220b, 0x0015210a, 0x0015210a, 0x01162108, 0x01172008,
	0x01182007, 0x02191f06, 0x02191f06, 0x021a1e06, 0x031a1e05,
	0x031c1d04, 0x041c1c04, 0x041d1c03, 0x051d1b03, 0x051e1a03,
	0x061f1902, 0x061f1902, 0x07201801, 0x08201701, 0x08211601,
	0x09211501, 0x0a211500, 0x0b211400, 0x0b221300, 0x0c221200,
	0x0d221100, 0x0e221000,
	/* counter = 11 */
	0x0010210f, 0x0011210e, 0x0011210e, 0x0012210d, 0x0013210c,
	0x0014200c, 0x0114200b, 0x0115200a, 0x01161f0a, 0x01171f09,
	0x02171f08, 0x02181e08, 0x03181e07, 0x031a1d06, 0x031a1d06,
	0x041b1c05, 0x041c1c04, 0x051c1b04, 0x051d1a04, 0x061d1a03,
	0x071d1903, 0x071e1803, 0x081e1802, 0x081f1702, 0x091f1602,
	0x0a201501, 0x0b1f1501, 0x0b201401, 0x0c211300, 0x0d211200,
	0x0e201200, 0x0e211100,
	/* counter = 12 */
	0x00102010, 0x0011200f, 0x0012200e, 0x0013200d, 0x0013200d,
	0x01141f0c, 0x01151f0b, 0x01151f0b, 0x01161f0a, 0x02171e09,
	0x02171e09, 0x03181d08, 0x03191d07, 0x03191d07, 0x041a1c06,
	0x041b1c05, 0x051b1b05, 0x051c1b04, 0x061c1a04, 0x071d1903,
	0x071d1903, 0x081d1803, 0x081e1703, 0x091e1702, 0x0a1f1601,
	0x0a1f1502, 0x0b1f1501, 0x0c1f1401, 0x0d201300, 0x0d201300,
	0x0e201200, 0x0f201100,
	/* counter = 13 */
	0x00102010, 0x0011200f, 0x00121f0f, 0x00131f0e, 0x00141f0d,
	0x01141f0c, 0x01141f0c, 0x01151e0c, 0x02161e0a, 0x02171e09,
	0x03171d09, 0x03181d08, 0x03181d08, 0x04191c07, 0x041a1c06,
	0x051a1b06, 0x051b1b05, 0x061b1a05, 0x061c1a04, 0x071c1904,
	0x081c1903, 0x081d1803, 0x091d1703, 0x091e1702, 0x0a1e1602,
	0x0b1e1502, 0x0c1e1501, 0x0c1f1401, 0x0d1f1400, 0x0e1f1300,
	0x0e1f1201, 0x0f1f1200,
	/* counter = 14 */
	0x00111e11, 0x00121e10, 0x00131e0f, 0x00131e0f, 0x01131e0e,
	0x01141d0e, 0x02151d0c, 0x02151d0c, 0x02161d0b, 0x03161c0b,
	0x03171c0a, 0x04171c09, 0x04181b09, 0x05181b08, 0x05191b07,
	0x06191a07, 0x061a1a06, 0x071a1906, 0x071b1905, 0x081b1805,
	0x091b1804, 0x091c1704, 0x0a1c1703, 0x0a1c1604, 0x0b1d1602,
	0x0c1d1502, 0x0c1d1502, 0x0d1d1402, 0x0e1d1401, 0x0e1e1301,
	0x0f1e1300, 0x101e1200,
	/* counter = 15 */
	0x00111e11, 0x00121e10, 0x00131d10, 0x01131d0f, 0x01141d0e,
	0x01141d0e, 0x02151c0d, 0x02151c0d, 0x03161c0b, 0x03161c0b,
	0x04171b0a, 0x04171b0a, 0x05171b09, 0x05181a09, 0x06181a08,
	0x06191a07, 0x07191907, 0x071a1906, 0x081a1806, 0x081a1806,
	0x091a1805, 0x0a1b1704, 0x0a1b1704, 0x0b1c1603, 0x0b1c1603,
	0x0c1c1503, 0x0d1c1502, 0x0d1d1402, 0x0e1d1401, 0x0f1d1301,
	0x0f1d1301, 0x101e1200,
	/* counter = 16 */
};
_Static_assert(sizeof(linearcoefftab32)==32*4&&sizeof(lan2coefftab32_full)==512*4,
    "Pinned scaler coefficient banks");
/* Pinned g2d_vsu_calc_fir_coef: word offset of the horizontal phase bank for
 * a step with 19 fractional bits. Ratios of five or more share the last bank. */
static uint32_t fir_coef(uint32_t step)
{
    uint32_t ratio=step>>16,whole=ratio>>3,part=ratio&7;
    uint32_t bank=whole==0?1:whole==1?1+part:whole==2?9+(part>>1):whole==3?13:whole==4?14:15;
    return bank*32;
}
/* Pinned RGB g2d_ovl_v_calc_coarse: the video layer decimates a source more
 * than eight times wider or four times taller before the scaler. */
static uint32_t coarse(uint32_t in,uint32_t out,uint32_t factor,uint32_t *d0,uint32_t *d1)
{
    if(in<out*factor&&in<=2048)return in;
    uint32_t mid=in<out*factor?in:out*factor;
    if(mid>2048)mid=2048;
    *d0=*d1=(mid<<16)|in;return mid;
}
struct head { uint32_t low,len,dirty,offset; };
_Static_assert(sizeof(struct head)==16,"RCQ descriptor");
static void block(uint8_t *memory,unsigned h,unsigned at,unsigned size,unsigned offset)
{
    struct head *head=(void *)memory;
    head[h]=(struct head){A733_G2D_RCQ_IOVA+at,size,1,offset};
}
int a733_g2d_commands(void *memory,size_t capacity,const struct a733_g2d_session *s)
{
    if(!memory||(uintptr_t)memory%32||capacity!=4096||!a733_g2d_geometry_valid(s))return 0;
    memset(memory,0,capacity);uint8_t *p=memory;
    struct g2d_mixer_ovl_v_reg *v=(void *)(p+128);
    struct g2d_mixer_bld_reg *b=(void *)(p+384);
    struct g2d_mixer_ovl_u_reg *u=(void *)(p+256);
    struct g2d_mixer_write_back_reg *w=(void *)(p+832);
    unsigned scale=s->op==A733_G2D_SCALE2X||s->op==A733_G2D_SCALE;
    unsigned blend=s->op==A733_G2D_SRCOVER_SOLID||s->op==A733_G2D_SRCOVER_IMAGE;
    uint32_t sw=s->op==A733_G2D_SCALE2X?s->w/2:s->op==A733_G2D_SCALE?s->sw:s->w;
    uint32_t sh=s->op==A733_G2D_SCALE2X?s->h/2:s->op==A733_G2D_SCALE?s->sh:s->h;
    unsigned size=(s->w-1)|((s->h-1)<<16);
    unsigned source_size=(sw-1)|((sh-1)<<16);
    /* g2d_vlayer_set: format, alpha mode, plane alpha and enable. */
    v->ovl_attr.dwval=UINT32_C(0xff000001)|(s->op==1?16:0)|(s->format<<8);
    v->ovl_mem.dwval=source_size;v->ovl_winsize.dwval=source_size;
    v->ovl_mem_pitch0=s->stride;
    v->ovl_mem_low_addr0=A733_G2D_SOURCE_IOVA+s->sy*s->stride+s->sx*4;
    v->ovl_fill_color=s->color;
    b->bld_en_ctrl.dwval=0x101;b->mem_size[0].dwval=size;
    b->out_size.dwval=size;b->bld_ctrl.dwval=0x00010001; /* COPY */
    b->rop_ctrl.dwval=0xf0;b->ch3_index0.dwval=0x41000;
    if(blend){
        /* Effective g2d_bsp_bld: V0/pipe0 source, UI2/pipe1 destination.
         * bpremul=0 preserves straight alpha on both inputs and the output. */
        _Static_assert(sizeof(*u)==32,"UI2 RCQ slot ends before VSU");
        u->ovl_attr.dwval=UINT32_C(0xff000011);
        u->ovl_mem.dwval=size;u->ovl_winsize.dwval=size;
        u->ovl_mem_pitch0=s->stride;u->ovl_fill_color=s->color;
        if(s->op==A733_G2D_SRCOVER_IMAGE){
            /* G2D_MIXER_ALPHA multiplies foreground pixel and plane alpha.
             * The background reads its per-pixel alpha from the same upload. */
            v->ovl_attr.dwval=(s->alpha<<24)|4|1;
            u->ovl_attr.dwval=UINT32_C(0xff000001);
            u->ovl_mem_low_addr0=A733_G2D_SOURCE_IOVA+s->by*s->stride+s->bx*4;
        }
        b->bld_en_ctrl.dwval=0x301;b->mem_size[1].dwval=size;
        b->bld_ctrl.dwval=0x03010301; /* SRCOVER; straight output. */
    }
    if(scale){
        struct g2d_mixer_video_scaler_reg *vs=(void *)(p+1024);
        _Static_assert(1024+sizeof(*vs)<=4096,"VSU fits retained RCQ extent");
        /* Same RGB branch and filter banks as effective g2d_vsu_para_set,
         * after the pinned coarse decimation for large reductions. */
        uint32_t midw=coarse(sw,s->w,8,&v->hor_down_sample0.dwval,&v->hor_down_sample1.dwval);
        uint32_t midh=coarse(sh,s->h,4,&v->ver_down_sample0.dwval,&v->ver_down_sample1.dwval);
        uint32_t hstep=(midw<<19)/s->w,vstep=(midh<<19)/s->h;
        uint32_t mid=(midw-1)|((midh-1)<<16);
        vs->vs_ctrl.dwval=0x101;vs->out_size.dwval=size;
        vs->glb_alpha.dwval=255;
        vs->y_ch_size.dwval=vs->c_ch_size.dwval=mid;
        vs->y_hor_step.dwval=vs->c_hor_step.dwval=hstep<<1;
        vs->y_ver_step.dwval=vs->c_ver_step.dwval=vstep<<1;
        memcpy(vs->vs_y_ch_hor_filter_coef,lan2coefftab32_full+fir_coef(hstep),32*4);
        memcpy(vs->vs_c_ch_hor_filter_coef,lan2coefftab32_full+fir_coef(hstep),32*4);
        memcpy(vs->vs_y_ch_ver_filter_coef,linearcoefftab32,sizeof(linearcoefftab32));
    }
    w->wb_attr.dwval=s->output_format;
    w->data_size.dwval=size;w->pitch0=s->stride;
    w->laddr0=A733_G2D_OUTPUT_IOVA+s->dy*s->stride+s->dx*4;
    block(p,0,128,sizeof(*v),G2D_V0);
    block(p,1,192,4,G2D_UI0);block(p,2,224,4,G2D_UI1);
    block(p,3,256,blend?sizeof(*u):4,G2D_UI2);block(p,4,scale?1024:288,
        scale?sizeof(struct g2d_mixer_video_scaler_reg):4,G2D_VSU);
    block(p,5,384,sizeof(*b),G2D_BLD);block(p,6,832,sizeof(*w),G2D_WB);
    /* Eighth header is the required even-count zero-dirty terminator. */
    return 1;
}
