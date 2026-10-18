/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register/table formats adapted from the A733 Linux IOMMU driver.
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Copyright (C) 2016-2018, Allwinner Technology CO., LTD.
 * Author: zhuxianbin <zhuxianbin@allwinnertech.com>
 * Copyright (c) 2023, ouyangkun <ouyangkun@allwinnertech.com>
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License.
 */
#include "a733_iommu.h"
#define LIMIT (UINT64_C(1)<<34)
#define PAGE 4096u
#define DIRECTORY_WORDS 4096u
#define L2_WORDS 1024u
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
static const struct a733_iommu_master masters[] = {
    {"USB",0,0,0,0x400}, {"CSI",1,0,1,0x600},
    {"ISP",2,0,2,0x800}, {"VE_ENC",3,0,3,0xa00},
    {"VE_DEC0",6,1,0,0x10400}, {"VE_DEC1",7,1,1,0x10600},
    {"DE0",8,1,2,0x10800}, {"DI",9,1,3,0x10a00},
    {"G2D",10,1,4,0x10c00}, {"EINK",11,1,5,0x10e00}
};
const struct a733_iommu_master *a733_iommu_master(unsigned id)
{
    for(unsigned i=0;i<ARRAY_SIZE(masters);++i)if(masters[i].id==id)return &masters[i];
    return NULL;
}
static int overlaps(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{
    return an>UINT64_MAX-a || bn>UINT64_MAX-b || (a<b+bn && b<a+an);
}
static int aliases(const void *a,size_t an,const void *b,size_t bn)
{
    return overlaps((uintptr_t)a,an,(uintptr_t)b,bn);
}
static int bounds(uint64_t a,uint64_t n,uint64_t minimum)
{
    return a>=minimum && a<LIMIT && n && n<=LIMIT-a && !(a&(PAGE-1)) && !(n&(PAGE-1));
}
/* Returns number of unique L2 windows, first-use order. */
static int windows(const struct a733_iommu_tables *t,uint16_t slots[4])
{
    unsigned used=0;
    if(!t || !t->words || ((uintptr_t)t->words&16383) ||
       (t->physical&16383) || !bounds(t->physical,A733_IOMMU_TABLE_BYTES,0x40000000) ||
       !t->count || t->count>A733_IOMMU_MAX_SPANS)return -1;
    if(aliases(t,sizeof(*t),t->words,A733_IOMMU_TABLE_BYTES))return -1;
    for(unsigned i=0;i<t->count;++i) {
        const struct a733_iommu_span *p=&t->spans[i];
        if(!bounds(p->iova,p->bytes,PAGE) || !bounds(p->physical,p->bytes,0x40000000) ||
           !p->access || (p->access&~3u) ||
           overlaps(p->physical,p->bytes,t->physical,A733_IOMMU_TABLE_BYTES))return -1;
        for(unsigned j=0;j<i;++j)
            if(overlaps(p->iova,p->bytes,t->spans[j].iova,t->spans[j].bytes) ||
               overlaps(p->physical,p->bytes,t->spans[j].physical,t->spans[j].bytes))return -1;
        uint64_t first=p->iova>>22,last=(p->iova+p->bytes-1)>>22;
        if(last-first>=A733_IOMMU_L2_TABLES)return -1;
        for(uint64_t w=first;w<=last;++w) {
            unsigned k=0;while(k<used && slots[k]!=w)++k;
            if(k==used) {
                if(used==A733_IOMMU_L2_TABLES)return -1;
                slots[used++]=(uint16_t)w;
            }
        }
    }
    return (int)used;
}
static uint32_t address(uint64_t pa)
{
    return ((uint32_t)pa&0xfffff000u)|(uint32_t)((pa>>24)&0x300u);
}
static uint32_t leaf(uint64_t pa,unsigned access)
{
    return address(pa)|2u|((access&1u)?4u:0)|((access&2u)?8u:0);
}
static uint32_t expected(const struct a733_iommu_tables *t,const uint16_t *slots,
                         unsigned used,unsigned index)
{
    if(index<DIRECTORY_WORDS) {
        for(unsigned k=0;k<used;++k)if(slots[k]==index)
            return address(t->physical+16384+k*PAGE)|1u;
        return 0;
    }
    unsigned k=(index-DIRECTORY_WORDS)/L2_WORDS;
    if(k>=used)return 0;
    uint64_t va=((uint64_t)slots[k]<<22)+((index%L2_WORDS)*PAGE);
    for(unsigned i=0;i<t->count;++i) {
        const struct a733_iommu_span *p=&t->spans[i];
        if(va>=p->iova && va-p->iova<p->bytes)
            return leaf(p->physical+va-p->iova,p->access);
    }
    return 0;
}
int a733_iommu_build(struct a733_iommu_tables *out,uint32_t *words,uint64_t pa,
                    const struct a733_iommu_span *spans,unsigned count)
{
    struct a733_iommu_tables t={.words=words,.physical=pa,.count=count};
    uint16_t slots[4];
    if(!out || !words || !spans || !count || count>A733_IOMMU_MAX_SPANS ||
       aliases(out,sizeof(*out),words,A733_IOMMU_TABLE_BYTES) ||
       aliases(spans,count*sizeof(*spans),words,A733_IOMMU_TABLE_BYTES) ||
       aliases(out,sizeof(*out),spans,count*sizeof(*spans)))return -1;
    for(unsigned i=0;i<count;++i)t.spans[i]=spans[i];
    int used=windows(&t,slots);if(used<0)return -1;
    for(unsigned i=0;i<A733_IOMMU_TABLE_BYTES/4;++i)words[i]=expected(&t,slots,(unsigned)used,i);
    *out=t;return 1;
}
int a733_iommu_validate(const struct a733_iommu_tables *t)
{
    uint16_t slots[4];int used=windows(t,slots);if(used<0)return -1;
    for(unsigned i=0;i<A733_IOMMU_TABLE_BYTES/4;++i)
        if(t->words[i]!=expected(t,slots,(unsigned)used,i))return -1;
    return 1;
}
int a733_iommu_translate(const struct a733_iommu_tables *t,uint64_t va,
                        unsigned access,uint64_t *pa)
{
    if(!pa || !access || (access&~3u) || va>=LIMIT || a733_iommu_validate(t)!=1 ||
       aliases(pa,sizeof(*pa),t,sizeof(*t)) ||
       aliases(pa,sizeof(*pa),t->words,A733_IOMMU_TABLE_BYTES))return -1;
    for(unsigned i=0;i<t->count;++i) {
        const struct a733_iommu_span *p=&t->spans[i];
        if(va>=p->iova && va-p->iova<p->bytes && (p->access&access)==access) {
            *pa=p->physical+va-p->iova;return 1;
        }
    }
    return -1;
}
static int fail(struct a733_iommu *s) { s->state=A733_IOMMU_FAULT;return -1; }
static int tick(struct a733_iommu *s)
{
    uint64_t n;
    if(s->ops.now_us(s->context,&n)!=1 || n<s->now || n>=s->deadline)return fail(s);
    s->now=n;return 1;
}
static int read_reg(struct a733_iommu *s,uint32_t off,uint32_t *v)
{
    if(s->ops.read(s->context,s->admission.token,off,v,s->deadline)!=1 || tick(s)!=1)return fail(s);
    return 1;
}
static int write_reg(struct a733_iommu *s,uint32_t off,uint32_t v)
{
    if(s->ops.write(s->context,s->admission.token,off,v,s->deadline)!=1 || tick(s)!=1)return fail(s);
    return 1;
}
static int admitted(const struct a733_iommu *s)
{
    const struct a733_iommu_admission *a=&s->admission;
    return a->token && a->excluded_masters==A733_IOMMU_REAL_MASTERS &&
        a->powered_masters==s->accessible_masters && a->inherited_resolved==1 &&
        a->transactions_drained==1 && a->clocks_held==1 &&
        (s->camera_supervised ? (a->recovery_qualified==0 && a->retained_power_removal==1) :
         (a->recovery_qualified==1 || (s->translated_masters==0xc0 &&
          a->recovery_qualified==0 && a->retained_power_removal==1)));
}
int a733_iommu_init(struct a733_iommu *s,const struct a733_iommu_ops *ops,
                    void *ctx,const struct a733_iommu_tables *t)
{
    if(!s || !ops || !t || s->initialized || s->state!=A733_IOMMU_NEW ||
       !ops->now_us || !ops->admit || !ops->read || !ops->write || !ops->publish ||
       aliases(s,sizeof(*s),ops,sizeof(*ops)) || aliases(s,sizeof(*s),t,sizeof(*t)) ||
       aliases(s,sizeof(*s),t->words,A733_IOMMU_TABLE_BYTES) ||
       (ctx && aliases(s,sizeof(*s),ctx,1)) || a733_iommu_validate(t)!=1)return -1;
    /* Camera allocation policy is narrower than the hardware's 34-bit IOVA. */
    for(unsigned i=0;i<t->count;++i)
        if(t->spans[i].iova+t->spans[i].bytes>UINT64_C(0x100000000))return -1;
    *s=(struct a733_iommu){.ops=*ops,.tables=*t,.context=ctx,.initialized=1,
        .translated_masters=2,.accessible_masters=A733_IOMMU_REAL_MASTERS};return 1;
}
int a733_iommu_init_camera_supervised(struct a733_iommu *s,const struct a733_iommu_ops *ops,
    void *ctx,const struct a733_iommu_tables *tables)
{
    int r=a733_iommu_init(s,ops,ctx,tables);
    if(r==1)s->camera_supervised=1;
    return r;
}
int a733_iommu_init_decoder(struct a733_iommu *s,const struct a733_iommu_ops *ops,
    void *ctx,const struct a733_iommu_tables *tables)
{
    int r=a733_iommu_init(s,ops,ctx,tables);
    if(r==1){s->translated_masters=0xc0;s->accessible_masters=0xc0;}
    return r;
}
/* The pinned v2 source skips unavailable microsets in its MMIO wrappers.
 * No skipped status is interpreted as an observation. Global bank registers
 * remain accessible under the retained IOMMU clocks and exclusion contract. */
static int accessible(const struct a733_iommu *s,unsigned master)
{return !!(s->accessible_masters & (1u<<master));}
int a733_iommu_open(struct a733_iommu *s,uint64_t d)
{
    if(!s || !s->initialized || s->state!=A733_IOMMU_NEW || !d || d<=s->now)return -1;
    s->deadline=d;s->state=A733_IOMMU_ADMIT;return 1;
}
/* Each ordinary write receives its own readback; flush request writes have no
 * invented completion bit. kind1 waits, kind2 writes without readback. */
struct operation { uint32_t off,value; unsigned kind; };
static struct operation operation(unsigned i,uint64_t table,uint16_t translated)
{
    struct operation p={0,0,0};
    if(i<2) { p.off=i*0x10000+0x20;p.value=(uint32_t)(table>>14); }
    else if(i<4) { p.off=(i-2)*0x10000+0x2c;p.value=0x3007f; }
    else if(i<6) { p.off=(i-4)*0x10000+0x40;p.value=1; }
    else if(i<8) { p.off=(i-6)*0x10000+0x60;p.value=1; }
    else if(i<10) { p.off=(i-8)*0x10000+0x80;p.value=0; }
    else if(i<20) { p.off=masters[i-10].micro_offset+0x30;p.value=0; }
    else if(i<30) { p.off=masters[i-20].micro_offset+4;p.value=!(translated&(1u<<masters[i-20].id)); }
    else if(i<32) { p.off=(i-30)*0x10000+0x30;p.value=3;p.kind=2; }
    else if(i<42) { p.off=masters[i-32].micro_offset+0x18;p.value=1;p.kind=2; }
    else if(i==42) { p.kind=1; }
    else if(i<45) { p.off=(i-43)*0x10000+0x30;p.value=0; }
    else if(i<47) { p.off=(i-45)*0x10000+0x1c;p.value=0x7fff0003; }
    else { p.off=(i-47)*0x10000+0x14;p.value=1; }
    return p;
}
static int program(struct a733_iommu *s)
{
    uint32_t v;
    if(s->index==49) { s->state=A733_IOMMU_ACTIVE;return 1; }
    while(s->index<42) {
        unsigned i=s->index,m;
        if(i>=10&&i<20)m=i-10;
        else if(i>=20&&i<30)m=i-20;
        else if(i>=32)m=i-32;
        else break;
        if(accessible(s,masters[m].id))break;
        ++s->index;
    }
    struct operation p=operation(s->index,s->tables.physical,s->translated_masters);
    if(p.kind==1) {
        if(!s->phase) {
            if(s->now>UINT64_MAX-10)return fail(s);
            s->wait_until=s->now+10;s->phase=1;
        }
        if(s->now<s->wait_until)return 0;
    } else if(!s->phase) {
        if(write_reg(s,p.off,p.value)!=1)return -1;
        if(!p.kind) { s->phase=1;return 0; }
    } else {
        if(read_reg(s,p.off,&v)!=1)return -1;
        if(v!=p.value)return fail(s);
    }
    ++s->index;s->phase=0;return 0;
}
static int scan_step(struct a733_iommu *s)
{
    uint32_t *out;unsigned i=s->index,off;
    if(i<18) {
        unsigned b=i/9,k=i%9;off=b*0x10000;
        static const uint16_t offsets[]={0x88,0xb0,0xb4,0x8c,0x90,0x9c,0x94,0x98,0xa0};
        out=k==0?&s->fault.bank_status[b]:k==1?&s->fault.l1_status[b]:
            k==2?&s->fault.l2_status[b]:k==3?&s->fault.walk_address_low[b][0]:
            k==4?&s->fault.walk_address_high[b][0]:k==5?&s->fault.walk_data[b][0]:
            k==6?&s->fault.walk_address_low[b][1]:k==7?&s->fault.walk_address_high[b][1]:
            &s->fault.walk_data[b][1];off+=offsets[k];
    } else if(i<58) {
        unsigned m=(i-18)/4,k=(i-18)%4;
        if(!accessible(s,masters[m].id)){s->index+=4-k;return 0;}
        s->fault.scanned_masters|=(uint16_t)(1u<<masters[m].id);
        off=masters[m].micro_offset+0x38+k*4;
        out=k==0?&s->fault.micro_status[m]:k==1?&s->fault.micro_low[m]:
            k==2?&s->fault.micro_high[m]:&s->fault.micro_data[m];
    } else {
        s->fault.complete=1;
        if(s->phase || s->fault.bank_status[0] || s->fault.bank_status[1] ||
           s->fault.l1_status[0] || s->fault.l1_status[1] ||
           s->fault.l2_status[0] || s->fault.l2_status[1])return fail(s);
        for(unsigned m=0;m<10;++m)if(s->fault.micro_status[m])return fail(s);
        s->state=A733_IOMMU_ACTIVE;return 1;
    }
    if(read_reg(s,off,out)!=1)return -1;
    ++s->index;return 0;
}
int a733_iommu_scan(struct a733_iommu *s,uint64_t d)
{
    if(!s || !s->initialized || !admitted(s) || !d || d<=s->now ||
       (s->state!=A733_IOMMU_ACTIVE && s->state!=A733_IOMMU_FAULT))return -1;
    s->phase=s->state==A733_IOMMU_FAULT;s->deadline=d;s->index=0;
    s->fault=(struct a733_iommu_fault){0};s->state=A733_IOMMU_SCAN;return 1;
}
int a733_iommu_poll(struct a733_iommu *s)
{
    uint32_t v;int r;
    if(!s || !s->initialized || s->state==A733_IOMMU_NEW || s->state==A733_IOMMU_FAULT)return -1;
    if(s->state==A733_IOMMU_ACTIVE)return 1;
    if(tick(s)!=1)return -1;
    switch(s->state) {
    case A733_IOMMU_ADMIT: {
        uint64_t token=s->admission.token;
        r=s->ops.admit(s->context,&s->tables,s->deadline,&s->admission);
        if(token && s->admission.token!=token) { s->admission.token=token;return fail(s); }
        if(r<0 || r>1 || tick(s)!=1)return fail(s);
        if(!r)return 0;
        if(!admitted(s) || a733_iommu_validate(&s->tables)!=1)return fail(s);
        s->state=A733_IOMMU_CHECK;return 0;
    }
    case A733_IOMMU_CHECK:
        /* Enabled inherited mappings are never replaced, even with a token. */
        if(s->index<2) {
            if(read_reg(s,s->index*0x10000+0x14,&v)!=1)return -1;
        } else if(s->index<4) {
            if(read_reg(s,(s->index-2)*0x10000+0x88,&v)!=1)return -1;
        } else if(s->index<8) {
            unsigned k=s->index-4;
            if(read_reg(s,(k/2)*0x10000+0xb0+(k%2)*4,&v)!=1)return -1;
        } else if(s->index<18) {
            if(!accessible(s,masters[s->index-8].id)){++s->index;return 0;}
            if(read_reg(s,masters[s->index-8].micro_offset+0x38,&v)!=1)return -1;
        } else { s->state=A733_IOMMU_PUBLISH;return 0; }
        if(v)return fail(s);
        ++s->index;return 0;
    case A733_IOMMU_PUBLISH:
        r=s->ops.publish(s->context,s->admission.token,s->tables.words,A733_IOMMU_TABLE_BYTES,s->deadline);
        if(r<0 || r>1 || tick(s)!=1)return fail(s);
        if(r) { s->index=0;s->state=A733_IOMMU_PROGRAM; }return 0;
    case A733_IOMMU_PROGRAM:return program(s);
    case A733_IOMMU_SCAN:return scan_step(s);
    default:return fail(s);
    }
}
