/* SPDX-License-Identifier: GPL-2.0-only
 * Adapted from Allwinner pck600_domains.c, Linux revision
 * 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (c) 2021 ALLWINNER, Co. Ltd.
 * Source, hash and adaptation scope: apps/g2d/sources.json.
 */
#include "a733_de_sys_power.h"
enum { INITIAL,WAIT,READY,FAILED };
static int fail(struct a733_de_sys_power *p){p->state=FAILED;return -1;}
static int timely(struct a733_de_sys_power *p)
{
    uint64_t t;
    if(p->ops.now(p->context,&t)!=1||t<p->last_now||t>=p->deadline)return 0;
    p->last_now=t;return p->state!=WAIT||t<p->wait_deadline;
}
static int read32(struct a733_de_sys_power *p,unsigned off,uint32_t *v)
{return timely(p)&&p->ops.read(p->context,A733_DE_SYS_PCK_BASE+off,v)==1&&timely(p);}
static int write32(struct a733_de_sys_power *p,unsigned off,uint32_t v)
{
    if(!timely(p))return 0;
    p->write_attempted=1;
    return p->ops.write(p->context,A733_DE_SYS_PCK_BASE+off,v)==1&&timely(p);
}
static int release(struct a733_de_sys_power *p)
{p->ops.leave(p->context);p->held=0;return timely(p);}
int a733_de_sys_power_init(struct a733_de_sys_power *p,
    const struct a733_de_sys_power_ops *o,void *v,uint64_t d)
{
    if(!p||p->self||!o||!o->authorize||!o->now||!o->enter||!o->leave||
       !o->read||!o->write||!d)return 0;
    p->self=p;p->ops=*o;p->context=v;p->deadline=d;return 1;
}
int a733_de_sys_power_ready(const struct a733_de_sys_power *p)
{return p&&p->self==p&&p->state==READY&&!p->held;}
int a733_de_sys_power_poll(struct a733_de_sys_power *p)
{
    if(!p||p->self!=p)return -1;
    if(p->state==FAILED)return -1;
    if(!timely(p))return fail(p);
    int r=p->ops.authorize(p->context);
    if(r<0)return fail(p);if(!r)return timely(p)?0:fail(p);
    if(!timely(p))return fail(p);
    if(p->state==READY)return 1;
    r=p->ops.enter(p->context,p->state==WAIT?p->wait_deadline:p->deadline);
    if(r<0)return fail(p);if(!r)return timely(p)?0:fail(p);p->held=1;
    uint32_t command,status;
    if(!read32(p,0,&command)||!read32(p,8,&status))return fail(p);
    if(p->state==INITIAL){
        if((command&15)!=(status&15)||((status&15)!=0&&(status&15)!=8))return fail(p);
        if((status&15)==8){
            p->inherited_on=1;if(!release(p))return fail(p);
            p->state=READY;return 1;
        }
        static const unsigned offsets[]={0x170,0x174,0xc00,0xc04,0xc10};
        static const uint32_t values[]={0x1f1f1f,0x1f1f,0x08080808,0x0808,8};
        for(unsigned i=0;i<5;++i){
            uint32_t v;
            if(!write32(p,offsets[i],values[i])||!read32(p,offsets[i],&v)||v!=values[i])return fail(p);
        }
        /* Set the bound before the write can take effect. WAIT permits our
         * command8/status0 transition; initial incoherence was refused above. */
        if(!timely(p)||p->last_now>UINT64_MAX-10000)return fail(p);
        p->wait_deadline=p->last_now+10000;
        if(p->wait_deadline>p->deadline)p->wait_deadline=p->deadline;
        p->state=WAIT;
        if(!write32(p,0,(command&~UINT32_C(15))|8)||
           !read32(p,0,&status)||status!=((command&~UINT32_C(15))|8))return fail(p);
        if(!release(p))return fail(p);return 0;
    }
    if((command&15)!=8||((status&15)!=0&&(status&15)!=8))return fail(p);
    if(!release(p))return fail(p);
    if((status&15)==8)p->state=READY;
    return p->state==READY?1:0;
}
