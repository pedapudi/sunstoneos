/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register layout and setup adapted from the published display driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2017 Allwinner.
 */
#include "a733_display_engine.h"
#include <stddef.h>
#include <string.h>
#define DE A733_DE_BASE
#define OVL UINT32_C(0x221000)
#define BLD UINT32_C(0x2a1000)
#define COUNT(a) (sizeof(a) / sizeof(*(a)))
struct word { uint32_t offset, mask, value; };
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y || (x < y + bn && y < x + an);
}
static int plan(unsigned width,unsigned height,unsigned pixel_bytes,unsigned format,
                const struct a733_de_surface *s,struct a733_de_plan *out)
{
    if(!s||!out||overlap(s,sizeof(*s),out,sizeof(*out))||!s->owner||!s->device_address||
       (s->device_address&3)||(s->pitch&3)||s->pitch<pixel_bytes*width)return 0;
    uint64_t bytes=(uint64_t)s->pitch*height,limit=UINT64_C(1)<<40;
    if(!bytes||s->device_address>=limit||bytes>limit-s->device_address||
       s->mapped_bytes<bytes||s->mapped_bytes>limit-s->device_address)return 0;
    struct a733_de_plan p={0};
    p.size=(width-1)|((height-1)<<16);p.padded_bytes=bytes;
    p.last_pixel_address=s->device_address+bytes-s->pitch+pixel_bytes*width-1;
    p.overlay[0]=0xff000003|(format<<8);
    p.overlay[1]=p.size;p.overlay[3]=s->pitch;p.overlay[4]=(uint32_t)s->device_address;
    p.overlay[0x80/4]=(uint32_t)(s->device_address>>32);p.overlay[0x88/4]=p.size;
    p.blender[0]=0x101;p.blender[1]=0xff000000;p.blender[2]=p.size;
    p.blender[0x8c/4]=p.size;p.blender[0x90/4]=0x03010301;*out=p;return 1;
}
int a733_de_plan(const struct a733_tcon_lcd_timing *t,const struct a733_de_surface *s,
                 struct a733_de_plan *out)
{
    struct a733_tcon_lcd_plan lcd;
    if(!t||!out||overlap(t,sizeof(*t),out,sizeof(*out))||!a733_tcon_lcd_plan(t,&lcd))return 0;
    return plan(t->dsi.width,t->dsi.height,4,4,s,out);
}
int a733_de_plan_tv0(const struct a733_de_surface *s,struct a733_de_plan *out)
{return plan(1280,720,2,0x0a,s,out);}
int a733_de_init(struct a733_de *d, const struct a733_de_config *c, uint64_t deadline)
{
    if (!d || !c || overlap(d, sizeof(*d), c, sizeof(*c)) || d->initialized || !deadline ||
        !c->lease || !c->ops.admit || !c->ops.read32 || !c->ops.write32 ||
        !c->ops.barrier || !c->ops.now_us) return 0;
    struct a733_de_plan p;
    if(c->output==A733_DE_LCD1_XRGB8888){if(!a733_de_plan(&c->timing,&c->surface,&p))return 0;}
    else if(c->output==A733_DE_TV0_RGB565){if(!a733_de_plan_tv0(&c->surface,&p))return 0;}
    else return 0;
    memset(d, 0, sizeof(*d));
    d->config = *c; d->plan = p; d->deadline = deadline; d->initialized = 1;
    return 1;
}
int a733_de_request(struct a733_de *d, enum a733_de_action a, uint64_t deadline)
{
    if (!d || !d->initialized || d->entered || deadline <= d->last_now ||
        !((a == A733_DE_START && d->state == A733_DE_READY) ||
          ((a == A733_DE_OBSERVE || a == A733_DE_STOP) && d->state == A733_DE_ARMED))) return 0;
    d->action = a; d->deadline = deadline; d->state = A733_DE_PENDING;
    d->step = d->index = d->substep = 0;
    return 1;
}
static void fail(struct a733_de *d, enum a733_de_error error)
{
    d->error = error; d->state = A733_DE_FAULT_RETAINED;
}
static int time_ok(struct a733_de *d)
{
    uint64_t n = 0;
    if (d->config.ops.now_us(d->config.context, &n) != 1 || n < d->last_now || n >= d->deadline) {
        fail(d, A733_DE_TIME); return 0;
    }
    d->last_now = n;
    return d->state == A733_DE_PENDING;
}
/* Checks supplement authenticated cold/quiescent provenance. They do not
 * establish it. Complete shared-word exclusion also excludes autonomous RCQ. */
static const struct word common[] = {
    {0x8000, 0x10010, 0x10010}, {0x8004, 0x10010, 0x10010}, {0x8008, 0x11, 0x11},
    {0x8100, UINT32_MAX, 0}, {0x8110, 1, 0}, {0x8150, 1, 0},
    {0x80d0, 1, 0}, {0x8020, 1, 0},
    {0x220800, 1, 0}, {0x224000, 1, 0}, /* UCH3 CSC and scaler bypass. */
    {0x2a9000, 1, 0}, {0x2a9040, 0x1f, 0}, {0x2a9050, 1, 0},
    {0x2a9070, 1, 0}, {0x2a907c, 1, 0}, /* gamma/color test/CTC/demo/skin */
    {0x2a8000, UINT32_MAX, 0}, {0x2ab000, 1, 0}, /* dither, SMBL */
    {0x2a2800, UINT32_MAX, 0}, {0x2a2804, UINT32_MAX, 0}
};
static const struct word cold[] = {
    {0x8140, UINT32_MAX, 0}, {OVL, 1, 0}, {OVL+0x20, 1, 0},
    {OVL+0x40, 1, 0}, {OVL+0x60, 1, 0}, {BLD, 0x3f00, 0}
};
static const struct word setup[] = {
    {0x8010, 0xf0, 0x10}, {0x802c, 0xf000, 0x1000}, {0x8034, 0xf, 0xb},
    {0x8054, UINT32_MAX, 0x1000}, {0x8080, 0xffff0000, 0x08000000},
    {0x8148, UINT32_MAX, 0}, /* dimensions supplied by planner */
    {0x8154, UINT32_MAX, 0}, {0x8158, UINT32_MAX, 0}, {0x815c, UINT32_MAX, 0}
};
static const struct word start[] = {{0x814c, 1, 1}, {0x8140, 1, 1}};
static const struct word stop[] = {{0x8140, 1, 0}}; /* Leave clocks until external drain. */
/* Published AHB blocks, merging only the blender's overlapping 0xa0 word. */
static const struct { unsigned offset, words; } spans[] = {
    {OVL, 7}, {OVL+0x20, 7}, {OVL+0x40, 7}, {OVL+0x60, 7},
    {OVL+0x80, 3}, {OVL+0xe0, 7}, {BLD, 24}, {BLD+0x80, 32}
};
#define IMAGE_WORDS 94u
static struct word image_word(const struct a733_de *d, unsigned i)
{
    for (unsigned n = 0; n < COUNT(spans); ++n) {
        if (i < spans[n].words) {
            uint32_t offset = spans[n].offset + 4 * i;
            uint32_t value = offset < BLD ? d->plan.overlay[(offset-OVL)/4] : d->plan.blender[(offset-BLD)/4];
            return (struct word){offset, UINT32_MAX, value};
        }
        i -= spans[n].words;
    }
    return (struct word){0, 0, 0}; /* Unreachable with IMAGE_WORDS. */
}
static struct word write_word(const struct a733_de *d, unsigned i)
{
    if (d->action == A733_DE_START) return start[i];
    if (d->action == A733_DE_STOP) return stop[i];
    if (i < IMAGE_WORDS) return image_word(d, i);
    struct word w = setup[i - IMAGE_WORDS];
    if (w.offset == 0x8148) w.value = d->plan.size;
    if(w.offset==0x8010&&d->config.output==A733_DE_TV0_RGB565)w.value=0x40;
    return w;
}
/* DE352 RCQ: pinned de_base.h header encoding and sunxi_de.c aligned
 * header pairs; de_top.c output1 stride0x40, stale W1C and busy submission.
 * No CPU writes to either command page occur after bank publication. */
static int replay_shape(uint64_t base,uint64_t f)
{return base&&!(base&4095)&&f&&!(f&4095)&&base<=UINT64_C(0x100000000)-8192&&
 f<=(UINT64_C(0x100000000)-base-8192)/2;}
static void replay_words(uint32_t w[16],uint64_t base,uint64_t f,unsigned slot)
{
 memset(w,0,64);w[0]=(uint32_t)(base+2*f+slot*4096+32);
 w[1]=4;w[2]=1;w[3]=OVL+0x10;w[8]=(uint32_t)(base+slot*f);
}
int a733_de_replay_build(void *pages,uint64_t base,uint64_t f)
{
 if(!pages||((uintptr_t)pages&4095)||!replay_shape(base,f))return 0;
 memset(pages,0,8192);for(unsigned slot=0;slot<2;++slot){uint32_t words[16];
 replay_words(words,base,f,slot);memcpy((uint8_t *)pages+slot*4096,words,64);}return 1;
}
int a733_de_replay_valid(const void *pages,uint64_t base,uint64_t f)
{
 if(!pages||((uintptr_t)pages&4095)||!replay_shape(base,f))return 0;
 for(unsigned slot=0;slot<2;++slot){uint32_t words[16];replay_words(words,base,f,slot);
 const uint8_t *p=(const uint8_t *)pages+slot*4096;if(memcmp(p,words,64))return 0;
 for(unsigned i=64;i<4096;++i)if(p[i])return 0;}return 1;
}
int a733_de_replay_request(struct a733_de *d,unsigned slot,uint64_t f,uint64_t deadline)
{
 if(!d||!d->initialized||d->entered||d->state!=A733_DE_ARMED||slot>1||
 d->config.output!=A733_DE_LCD1_XRGB8888||!d->config.ops.rcq_submit||deadline<=d->last_now||
 !replay_shape(d->config.surface.device_address,f)||f<d->plan.padded_bytes||
 d->config.surface.mapped_bytes!=2*f+8192)return 0;
 d->replay_slot=slot;d->replay_frame_bytes=f;d->replay_rcq_done=d->replay_frame_end=0;
 d->deadline=deadline;d->state=A733_DE_PENDING;d->action=A733_DE_REPLAY;d->step=0;return 1;
}
static int replay_poll(struct a733_de *d)
{
 uint32_t v=0;int ok=0;void *c=d->config.context;unsigned step=d->step;
 uint64_t header=d->config.surface.device_address+2*d->replay_frame_bytes+4096*d->replay_slot;
 if(!step)ok=d->config.ops.admit(c,&d->config,A733_DE_REPLAY);
 else if(step==1)ok=d->config.ops.read32(c,DE+0x8140,&v);
 else if(step==2||step==4||step==13||step==15||step==17||step==18)ok=d->config.ops.read32(c,DE+0x8144,&v);
 else if(step==3||step==16)ok=d->config.ops.write32(c,DE+0x8144,(d->raw_status&~13u)|(step==3?5u:1u));
 else if(step==5)ok=d->config.ops.write32(c,DE+0x8154,(uint32_t)header);
 else if(step==6)ok=d->config.ops.write32(c,DE+0x8158,0);
 else if(step==7)ok=d->config.ops.write32(c,DE+0x815c,32);
 else if(step==8||step==12)ok=d->config.ops.barrier(c);
 else if(step==9){ok=d->config.ops.rcq_submit(c);if(ok==0)return time_ok(d);if(ok==1){d->step=12;return time_ok(d);}}
 else if(step==14)ok=d->config.ops.read32(c,DE+OVL+0x10,&v);
 if(ok!=1){fail(d,step?A733_DE_IO:A733_DE_AUTHORITY);return 0;}
 if(!time_ok(d))return 0;
 if(step==1&&!(v&1)){fail(d,A733_DE_INITIAL_STATE);return 0;}
 if(step==2||step==15)d->raw_status=v;
 if(step==4&&(v&5)){fail(d,A733_DE_VERIFY);return 0;}
 if(step==13){if(!(v&4))return 1;d->replay_rcq_done=d->last_now;}
 if(step==14&&v!=(uint32_t)(d->config.surface.device_address+d->replay_slot*d->replay_frame_bytes)){fail(d,A733_DE_VERIFY);return 0;}
 if(step==17&&(v&1)){fail(d,A733_DE_VERIFY);return 0;}
 if(step==18){if(!(v&1))return 1;d->raw_status=v;d->replay_frame_end=d->last_now;d->state=A733_DE_ARMED;}
 ++d->step;return 1;
}
enum a733_de_state a733_de_poll(struct a733_de *d)
{
    if (!d || !d->initialized) return A733_DE_FAULT_RETAINED;
    if (d->state != A733_DE_PENDING) return d->state;
    if (d->entered) { fail(d, A733_DE_REENTRY); return d->state; }
    d->entered = 1;
    if (!time_ok(d)) goto out;
    if(d->action==A733_DE_REPLAY){(void)replay_poll(d);goto out;}
    unsigned step = d->step;
    unsigned check_count = COUNT(common) + (d->action == A733_DE_PREPARE ? COUNT(cold) : 1);
    unsigned write_count = d->action == A733_DE_PREPARE ? IMAGE_WORDS + COUNT(setup) :
        d->action == A733_DE_START ? COUNT(start) : COUNT(stop);
    uint32_t value = 0;
    struct word w = {0};
    void *ctx = d->config.context;
    int ok = 0;
    if (!step) ok = d->config.ops.admit(ctx, &d->config, d->action);
    else if (step == 1) {
        if (d->index < COUNT(common)) w = common[d->index];
        else if (d->action == A733_DE_PREPARE) w = cold[d->index-COUNT(common)];
        else w = (struct word){0x8140, UINT32_MAX, d->action == A733_DE_START ? 0 : 1};
        ok = d->config.ops.read32(ctx, DE + w.offset, &value);
    } else if (step == 2) ok = d->config.ops.read32(ctx, DE + 0x8010, &value);
    else if (step == 3) {
        w = write_word(d, d->index);
        if (d->substep == 1) {
            ++d->writes_attempted; ok = d->config.ops.write32(ctx, DE + w.offset, d->expected);
        } else ok = d->config.ops.read32(ctx, DE + w.offset, &value);
    } else if (step == 4) ok = d->config.ops.barrier(ctx);
    else if (step == 5) ok = d->config.ops.read32(ctx, DE + 0x8144, &value);
    else { fail(d, A733_DE_INITIAL_STATE); goto out; }
    if (d->state != A733_DE_PENDING) goto out;
    if (ok != 1) { fail(d, step ? A733_DE_IO : A733_DE_AUTHORITY); goto out; }
    if (!time_ok(d)) goto out;
    if (step == 1) {
        if ((value & w.mask) != w.value) { fail(d, A733_DE_INITIAL_STATE); goto out; }
        if (++d->index < check_count) goto out;
        d->index = 0;
    } else if (step == 2) {
        unsigned route=d->config.output==A733_DE_TV0_RGB565?4u:1u;
        if ((value & 0xf) == route || ((value >> 8) & 0xf) == route || ((value >> 12) & 0xf) == route ||
            (d->action != A733_DE_PREPARE && (value & 0xf0) != (route<<4))) {
            fail(d, A733_DE_INITIAL_STATE); goto out;
        }
        if (d->action == A733_DE_OBSERVE) { d->step = 5; goto out; }
    } else if (step == 3) {
        if (!d->substep) d->expected = (value & ~w.mask) | w.value;
        if (d->substep == 2 && value != d->expected) { fail(d, A733_DE_VERIFY); goto out; }
        if (++d->substep < 3) goto out;
        d->substep = 0;
        if (++d->index < write_count) goto out;
    } else if (step == 4) {
        if (d->action == A733_DE_PREPARE) d->state = A733_DE_READY;
        if (d->action == A733_DE_STOP) d->state = A733_DE_DISABLED_RETAINED;
    } else if (step == 5) {
        d->raw_status = value; d->state = A733_DE_ARMED;
    }
    ++d->step;
out:
    d->entered = 0;
    return d->state;
}
