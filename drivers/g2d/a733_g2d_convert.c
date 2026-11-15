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
/* The RCQ command page of one YUV-to-RGB conversion with scaling (include/a733_g2d_convert.h states the job and its
 * refusals; docs/g2d-os.md#camera-frame-conversion). checks/g2d-convert-words.py compares every register word with a
 * transcription of the vendor functions. Integer only. */
#include "a733_g2d_convert.h"
#include "g2d_mixer_type.h"
#include <string.h>

/* g2d_scal.c lan2coefftab32_full: sixteen banks of 32 four-tap phases, the same bank as drivers/g2d/a733_g2d.c
 * (checks/g2d-convert-words.py compares the two copies). */
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

_Static_assert(sizeof(lan2coefftab32_full)==512*4,"Pinned scaler coefficient bank");
/* g2d_scal.c g2d_vsu_calc_fir_coef (248-268): the word offset of the bank for a step with 19 fraction bits. */
static uint32_t fir_coef(uint32_t step)
{
    const uint32_t ratio=step>>16,whole=ratio>>3,part=ratio&7;
    const uint32_t bank=whole==0?1:whole==1?1+part:whole==2?9+(part>>1):whole==3?13:whole==4?14:15;
    return bank*32;
}
/* g2d_bld.c Ycbcr2rgb_601 (37-50) and Ycbcr2rgb_709 (66-79), the blocks whose coefficients give full-range RGB. The
 * vendor's comments name the blocks by a source and destination range that their contents do not match: the block it
 * calls "full to limit" (offset 0) holds 1.164 (0x4a8) on Y and a constant of -222.9 for R, the limited-to-full
 * conversion, and the block it selects for a limited source and a full destination (offset 0x24, "limit to full")
 * holds 0.858 (0x36f), which compresses to limited RGB. The table is chosen here by its contents. */
static const int32_t csc[4][12]={
    [A733_G2D_CSC_BT601_LIMITED-1]={0x04a8,0x0,0x0662,(int32_t)0xFFFC8480,0x04a8,(int32_t)0xFFFFFE6F,
        (int32_t)0xFFFFFCC0,0x21E00,0x04a8,0x0812,0x0,(int32_t)0xFFFBAC80},               /* 601, offset 0 */
    [A733_G2D_CSC_BT601_FULL-1]={0x0400,0x0,0x059c,(int32_t)0xFFFD31FF,0x0400,(int32_t)0xFFFFFE9F,
        (int32_t)0xFFFFFD24,0x21D80,0x0400,0x0717,0x0,(int32_t)0xFFFC747F},               /* 601, offset 0x18 */
    [A733_G2D_CSC_BT709_LIMITED-1]={0x04a8,0x0,0x072c,(int32_t)0xFFFC1F80,0x04a8,(int32_t)0xFFFFFF26,
        (int32_t)0xFFFFFDDE,0x13380,0x04a8,0x0873,0,(int32_t)0xFFFB7C00},                 /* 709, offset 0 */
    [A733_G2D_CSC_BT709_FULL-1]={0x0400,0x0,0x64D,(int32_t)0xFFFC9980,0x0400,(int32_t)0xFFFFFF40,
        (int32_t)0xFFFFFE21,0x10F80,0x0400,0x076C,0x0,(int32_t)0xFFFC0A00},               /* 709, offset 0x18 */
};
const int32_t *a733_g2d_csc_table(uint32_t c){return c-1u<4u?csc[c-1u]:NULL;}
struct head { uint32_t low,len,dirty,offset; };
_Static_assert(sizeof(struct head)==16,"RCQ descriptor");
static void block(uint8_t *memory,uint32_t rcq,unsigned h,unsigned at,unsigned size,unsigned offset)
{
    ((struct head *)(void *)memory)[h]=(struct head){rcq+at,size,1,offset};
}
/* The first byte of a rectangle; at or above 2^32 when it cannot be written, since only the low address words are. */
static uint64_t origin(uint32_t base,uint32_t stride,uint32_t y,uint32_t x_bytes)
{
    return (uint64_t)base+(uint64_t)stride*y+x_bytes;
}
int a733_g2d_convert_commands(void *memory,size_t capacity,const struct a733_g2d_convert *c,uint32_t rcq_iova)
{
    if(!memory||!c||(uintptr_t)memory%32||capacity!=4096||(rcq_iova&4095))return 0;
    const uint32_t w=c->w,h=c->h,ow=c->ow,oh=c->oh;
    const int32_t *table=a733_g2d_csc_table(c->csc);
    if((c->format!=A733_G2D_YUV422_SP_A&&c->format!=A733_G2D_YUV422_SP_B)||c->out_format>7||!table||
       w-1u>=A733_G2D_CONVERT_MAX||h-1u>=A733_G2D_CONVERT_MAX||ow-1u>=A733_G2D_CONVERT_MAX||
       oh-1u>=A733_G2D_CONVERT_MAX||(c->sx&1)||c->luma_stride<w||c->chroma_stride<((w+1)&~1u)||
       (uint64_t)c->output_stride<(uint64_t)ow*4||((c->luma|c->chroma|c->output|c->luma_stride|c->chroma_stride|
       c->output_stride)&3))return 0;
    /* g2d_vlayer_set (g2d_ovl_v.c 244-335): one luma byte and one chroma pair (2 bytes) for two pixels, so the chroma
     * plane's rectangle starts at pitch1 * y + 2 * (x / 2). */
    const uint64_t luma=origin(c->luma,c->luma_stride,c->sy,c->sx);
    const uint64_t chroma=origin(c->chroma,c->chroma_stride,c->sy,c->sx);
    const uint64_t out=origin(c->output,c->output_stride,c->dy,c->dx*4u);
    if((luma|chroma|out)>>32)return 0;
    /* g2d_ovl_v_calc_coarse, the 4:2:2 semi-planar case (g2d_ovl_v.c 97-126): a source eight times wider or four
     * times taller than the output is decimated before the scaler, and the scaler reads at most 2048 columns. */
    uint32_t midw=w,midh=h,hd0=0,hd1=0,vd=0;
    if(w>=ow<<3){
        midw=ow<<3;if(midw>A733_G2D_VSU_MAX_INPUT)midw=A733_G2D_VSU_MAX_INPUT;
        hd0=midw<<16|w;hd1=midw<<15|((w+1)>>1);
    }else if(midw>A733_G2D_VSU_MAX_INPUT){
        midw=A733_G2D_VSU_MAX_INPUT;hd0=midw<<16|w;hd1=midw<<15|((w+1)>>1);
    }
    if(h>=oh<<2){midh=oh<<2;vd=midh<<16|h;}
    memset(memory,0,capacity);uint8_t *p=memory;
    struct g2d_mixer_ovl_v_reg *v=(void *)(p+128);
    struct g2d_mixer_bld_reg *b=(void *)(p+384);
    struct g2d_mixer_write_back_reg *wb=(void *)(p+832);
    struct g2d_mixer_video_scaler_reg *vs=(void *)(p+1024);
    _Static_assert(1024+sizeof(*vs)<=4096,"VSU fits the command page");
    _Static_assert(384+sizeof(*b)<=832&&128+sizeof(*v)<=192,"blocks do not overlap");
    const uint32_t size=(ow-1)|((oh-1)<<16);
    /* g2d_vlayer_set: format, alpha mode 0 (pixel alpha), plane alpha 0xff, the layer enabled; no fill colour. */
    v->ovl_attr.dwval=UINT32_C(0xff000001)|(c->format<<8);
    v->ovl_mem.dwval=v->ovl_winsize.dwval=(w-1)|((h-1)<<16);
    v->ovl_mem_pitch0=c->luma_stride;v->ovl_mem_pitch1=c->chroma_stride;
    v->ovl_mem_low_addr0=(uint32_t)luma;v->ovl_mem_low_addr1=(uint32_t)chroma;
    v->hor_down_sample0.dwval=hd0;v->hor_down_sample1.dwval=hd1;
    v->ver_down_sample0.dwval=vd;v->ver_down_sample1.dwval=vd;
    /* g2d_vsu_para_set, the 4:2:2 semi-planar case (g2d_scal.c 270-312, 361-383, 453-475): the scaler on with the
     * coefficient access and the YUV filter type, the luma steps at full rate, the chroma at half width with the
     * horizontal step register at half the luma's, Lanczos banks for both horizontal filters and the vertical one,
     * and every phase 0. */
    const uint32_t hstep=(midw<<19)/ow,vstep=(midh<<19)/oh,cw=(midw+1)>>1;
    vs->vs_ctrl.dwval=0x10101;vs->out_size.dwval=size;vs->glb_alpha.dwval=255;
    vs->y_ch_size.dwval=(midw-1)|((midh-1)<<16);
    vs->y_hor_step.dwval=hstep<<1;vs->y_ver_step.dwval=vstep<<1;
    vs->c_ch_size.dwval=(cw-1)|((midh-1)<<16);
    vs->c_hor_step.dwval=hstep;vs->c_ver_step.dwval=vstep<<1;
    memcpy(vs->vs_y_ch_hor_filter_coef,lan2coefftab32_full+fir_coef(hstep),32*4);
    memcpy(vs->vs_c_ch_hor_filter_coef,lan2coefftab32_full+fir_coef(hstep>>1),32*4);
    memcpy(vs->vs_y_ch_ver_filter_coef,lan2coefftab32_full+fir_coef(vstep),32*4);
    /* g2d_bsp_bitblt (g2d_mixer.c 304-371): source-over, ROP 0xf0, pipe 0 with its fill colour over the output rectangle, the blender in
     * YUV (bld_cs_set, g2d_bld.c 218-238), csc2 with the table and the fill colours black in YUV (bld_csc_reg_set,
     * g2d_bld.c 246-354),
     * and the output size (bld_out_setting, straight alpha). */
    b->bld_en_ctrl.dwval=0x101;b->bld_fill_color[0]=b->bld_fill_color[1]=0x00108080;
    b->mem_size[0].dwval=size;b->out_size.dwval=size;
    b->bld_ctrl.dwval=0x03010301;b->rop_ctrl.dwval=0xf0;b->ch3_index0.dwval=0x41000;
    b->out_color.dwval=2;b->cs_ctrl.dwval=4;
    memcpy(&b->csc2_coeff0_reg0,table,12*4);
    /* g2d_wb_set (g2d_wb.c 100-170): the output format, size, pitch and first pixel. */
    wb->wb_attr.dwval=c->out_format;wb->data_size.dwval=size;wb->pitch0=c->output_stride;wb->laddr0=(uint32_t)out;
    block(p,rcq_iova,0,128,sizeof(*v),G2D_V0);
    block(p,rcq_iova,1,192,4,G2D_UI0);block(p,rcq_iova,2,224,4,G2D_UI1);block(p,rcq_iova,3,256,4,G2D_UI2);
    block(p,rcq_iova,4,1024,sizeof(*vs),G2D_VSU);
    block(p,rcq_iova,5,384,sizeof(*b),G2D_BLD);block(p,rcq_iova,6,832,sizeof(*wb),G2D_WB);
    /* The eighth header is the even-count zero-dirty terminator. */
    return 1;
}
