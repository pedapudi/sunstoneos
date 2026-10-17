#include "sunxi_mmc.h"
#include "a733_mmc_platform_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define read32 original_read
#define write32 original_write
#include "mmc-card-model.h"
#undef read32
#undef write32
struct writer_model {
    struct model card;
    uint8_t media[4096],expected[4096];
    unsigned writes,full,programming,delay_program,post_busy,fail_command,fail_fifo;
    unsigned wp_error,no_complete,early_complete,late,authority_calls,revoke,mutate,change_source;
    uint64_t first,deadline;
    uint32_t blocks;
    const uint8_t *source;
};
static struct writer_model m;
static struct sunxi_mmc h;
static uint8_t source[4096];
static int barrier(void *v){(void)v;return 1;}
static int clock_now(void *v,uint64_t *out)
{struct writer_model *w=v;w->card.now+=50;*out=w->card.now;return 1;}
static int admit(void *v,uintptr_t token,enum sunxi_mmc_kind kind,uint32_t hz)
{struct writer_model *w=v;assert(token==77&&kind==(enum sunxi_mmc_kind)w->card.kind&&hz==800000);return 1;}
static int retained(void *v,uintptr_t token)
{assert(token==77);return ((struct writer_model *)v)->card.retain_result;}
static int authorization(void *v,const struct sunxi_mmc_write_identity *identity,
                         const struct sunxi_mmc_write_request *request)
{
    struct writer_model *w=v;++w->authority_calls;
    assert(identity->core==(uintptr_t)&h&&identity->session==77&&identity->kind==(enum sunxi_mmc_kind)w->card.kind);
    assert(identity->card.cid[0]==0x12345678&&identity->card.sectors==32);
    if(w->mutate==1)((struct sunxi_mmc_write_request *)request)->first_sector++;
    if(w->mutate==2)((struct sunxi_mmc_write_identity *)identity)->card.cid[0]++;
    if(w->mutate==3)h.card.cid[0]++;
    if(w->change_source){source[0]^=255;w->change_source=0;}
    if(w->late)w->card.now=request->deadline_us;
    return !w->revoke&&request->policy_token==901&&request->first_sector==w->first&&
        request->blocks==w->blocks&&request->source==w->source&&request->bytes==w->blocks*512u&&request->deadline_us==w->deadline;
}
static int read32(void *v,uint32_t off,uint32_t *out)
{
    struct writer_model *w=v;struct model *c=&w->card;
    if(off==0x3c&&(c->data_command&63u)==24) {
        ++c->calls;
        if(w->full&&c->fifo_pos<c->fifo_total){--w->full;*out=8;return 1;}
        *out=c->fifo_pos==c->fifo_total?4u:0u;
        if(c->fifo_pos==c->fifo_total&&w->post_busy){--w->post_busy;*out|=1u<<9;}
        return 1;
    }
    if(off==0x200)assert((c->data_command&63u)!=24);
    if(off==0x38&&w->no_complete)c->no_dtc=1;
    if(off==0x38&&w->early_complete&&(c->data_command&63u)==24)c->reg[off/4]|=8;
    return original_read(&w->card,off,out);
}
static int write32(void *v,uint32_t off,uint32_t value)
{
    struct writer_model *w=v;struct model *c=&w->card;
    if(off==0x200) {
        ++c->calls;assert((c->data_command&63u)==24&&c->fifo_pos+4<=512);
        assert(++c->fifo_this_poll<=16);
        unsigned at=(w->writes-1)*512+c->fifo_pos;
        for(unsigned i=0;i<4;++i)w->media[at+i]=(uint8_t)(value>>(i*8));
        c->fifo_pos+=4;
        if(w->fail_fifo&&c->fifo_pos>=w->fail_fifo)return 0;
        return 1;
    }
    if(off==0x18&&(value&63u)==24) {
        ++c->calls;assert(value==((1u<<31)|(1u<<13)|(1u<<10)|(1u<<9)|(1u<<8)|(1u<<6)|24));
        assert(c->selected&&c->reg[0x10/4]==512&&c->reg[0x14/4]==512);
        assert(w->writes<w->blocks&&(!w->writes||h.result.confirmed_blocks==w->writes));
        uint64_t sector=w->first+w->writes;
        assert(c->reg[0x1c/4]==(c->sector?sector:sector*512));
        c->commands[c->command_count]=24;c->args[c->command_count++]=c->reg[0x1c/4];
        c->reg[0x18/4]=value&~(1u<<31);c->reg[0x38/4]=4;
        c->reg[0x20/4]=w->wp_error?(1u<<26):0x900;
        c->data_command=value;c->fifo_total=512;c->fifo_pos=0;
        ++w->writes;w->programming=w->delay_program;
        return w->fail_command!=w->writes;
    }
    int post_status=off==0x18&&(value&63u)==13&&w->writes;
    int result=original_write(&w->card,off,value);
    if(post_status&&w->programming){--w->programming;c->reg[0x20/4]=7u<<9;}
    return result;
}
static void setup(unsigned kind,unsigned sector)
{
    memset(&m,0,sizeof(m));seed(&m.card,&h,kind,sector);
    const struct sunxi_mmc_config c={{read32,write32,barrier,clock_now,admit,retained},&m,77,
        (enum sunxi_mmc_kind)kind,800000,0x00300000};
    assert(sunxi_mmc_init(&h,&c));
    field(m.card.csd,84,12,21);field(m.card.csd,22,4,9);
    memcpy(h.card.cid,m.card.cid,16);memcpy(h.card.csd,m.card.csd,16);memcpy(h.ext_csd,m.card.ext,512);
    h.card.sectors=32;h.card.ocr=(1u<<31)|(sector?1u<<30:0)|0x00300000;
    h.card.rca=kind==SUNXI_MMC_SD?0x1234:1;h.card.sector_addressed=(uint8_t)sector;h.card.ext_revision=kind==SUNXI_MMC_EMMC?8:0;
    h.setup=6;h.admitted=1;h.state=SUNXI_MMC_READY;m.card.selected=1;m.card.state=4;
    for(unsigned i=0;i<4096;++i)source[i]=(uint8_t)(i*17+29);
    memcpy(m.expected,source,sizeof(source));m.first=2;m.blocks=2;m.source=source;m.deadline=1000000;
}
static struct sunxi_mmc_write_request request(void)
{return (struct sunxi_mmc_write_request){901,m.first,m.blocks,source,m.blocks*512u,m.deadline};}
static int start(void)
{struct sunxi_mmc_write_request r=request();struct sunxi_mmc_write_authority a={&m,authorization};return sunxi_mmc_write(&h,&r,&a);}
static void step(void){m.card.fifo_this_poll=0;sunxi_mmc_poll(&h);}
static void finish(void)
{for(unsigned n=0;n<5000&&h.state==SUNXI_MMC_WRITING;++n)step();assert(h.state==SUNXI_MMC_DONE||h.state==SUNXI_MMC_FAULT);}
static void fault_retained(void)
{
    assert(h.state==SUNXI_MMC_FAULT&&h.borrowed==source);
    struct sunxi_mmc_result result;unsigned calls=m.card.calls;
    assert(sunxi_mmc_take(&h,&result)&&h.borrowed==source);
    step();assert(calls==m.card.calls&&!start());
}
int main(void)
{
    /* Already-identified card fixtures reuse unchanged identification evidence. */
    for(unsigned kind=SUNXI_MMC_SD;kind<=SUNXI_MMC_EMMC;++kind) {
        setup(kind,kind==SUNXI_MMC_EMMC);m.blocks=kind==SUNXI_MMC_SD?2:8;
        m.full=2;m.delay_program=2;m.post_busy=3;m.change_source=1;
        assert(start());finish();assert(h.state==SUNXI_MMC_DONE);
        assert(h.result.submitted==m.blocks*512u&&h.result.confirmed_blocks==m.blocks&&h.result.write_started);
        assert(!memcmp(m.media,m.expected,m.blocks*512u));
        struct sunxi_mmc_result result;unsigned calls=m.card.calls;
        assert(sunxi_mmc_take(&h,&result)&&h.state==SUNXI_MMC_READY&&!h.borrowed&&calls==m.card.calls);
    }
    /* Validation is pure and default authority is denied. */
    setup(SUNXI_MMC_SD,1);
    struct sunxi_mmc_write_request r=request();struct sunxi_mmc_write_authority a={&m,authorization};
    assert(!sunxi_mmc_write(&h,&r,NULL));a.admit=0;assert(!sunxi_mmc_write(&h,&r,&a));a.admit=authorization;
    r.blocks=9;assert(!sunxi_mmc_write(&h,&r,&a));r=request();r.first_sector=31;assert(!sunxi_mmc_write(&h,&r,&a));
    r=request();r.policy_token=0;assert(!sunxi_mmc_write(&h,&r,&a));r=request();r.source=(void *)&h;assert(!sunxi_mmc_write(&h,&r,&a));
    assert(!m.card.calls&&!m.authority_calls&&!h.borrowed);
    setup(SUNXI_MMC_SD,0);r=request();r.first_sector=(uint64_t)UINT32_MAX/512+1;h.card.sectors=r.first_sector+2;
    assert(!sunxi_mmc_write(&h,&r,&a)&&!m.card.calls&&!h.borrowed);
    for(unsigned bad=0;bad<9;++bad) {
        setup(SUNXI_MMC_EMMC,1);
        if(bad==0)field(h.card.csd,13,1,1);
        if(bad==1)field(h.card.csd,12,1,1);
        if(bad==2)field(h.card.csd,84,12,5);
        if(bad==3)field(h.card.csd,22,4,10);
        if(bad==4)h.ext_csd[179]=1;
        if(bad==5)h.ext_csd[33]=1;
        if(bad==6)h.ext_csd[15]=1;
        if(bad==7)h.ext_csd[171]=4;
        if(bad==8)h.ext_csd[61]=1;
        assert(!start()&&!m.card.calls&&!h.borrowed);
    }
    for(unsigned bad=0;bad<5;++bad) {
        setup(SUNXI_MMC_SD,1);m.revoke=bad==0;m.mutate=bad>=1&&bad<=3?bad:0;m.late=bad==4;
        assert(start());step();assert(!m.card.calls&&!m.writes);fault_retained();
    }
    setup(SUNXI_MMC_EMMC,1);m.fail_command=2;assert(start());finish();
    assert(h.result.confirmed_blocks==1&&h.result.submitted==512&&m.writes==2&&h.result.write_started);fault_retained();
    setup(SUNXI_MMC_SD,1);m.fail_fifo=64;assert(start());finish();
    assert(h.result.submitted==64&&!h.result.confirmed_blocks);fault_retained();
    setup(SUNXI_MMC_SD,1);m.wp_error=1;assert(start());finish();
    assert(!h.result.submitted&&!h.result.confirmed_blocks);fault_retained();
    setup(SUNXI_MMC_SD,1);m.no_complete=1;m.deadline=3000;assert(start());finish();
    assert(h.result.error==SUNXI_MMC_TIMEOUT&&!h.result.confirmed_blocks);fault_retained();
    setup(SUNXI_MMC_SD,1);m.early_complete=1;assert(start());finish();
    assert(h.result.error==SUNXI_MMC_CONTROLLER&&!h.result.submitted&&!h.result.confirmed_blocks);fault_retained();
    setup(SUNXI_MMC_SD,1);assert(start());
    for(unsigned n=0;n<500&&!h.result.confirmed_blocks;++n)step();
    assert(h.result.confirmed_blocks==1&&m.writes==1);m.revoke=1;step();
    assert(m.writes==1);fault_retained();
    /* One affected read path, without replaying open/capacity suites. */
    setup(SUNXI_MMC_SD,1);uint8_t readback[512];
    assert(sunxi_mmc_read(&h,0,1,readback,sizeof(readback),1000000));
    for(unsigned n=0;n<1000&&h.state==SUNXI_MMC_READING;++n)step();
    assert(h.state==SUNXI_MMC_DONE&&h.result.received==512&&!h.result.write_started);
    for(unsigned n=0;n<512;++n)assert(readback[n]==(uint8_t)(n*37+11));
    for(unsigned kind=SUNXI_MMC_SD;kind<=SUNXI_MMC_EMMC;++kind)
        assert(!a733_mmc_platform_native_address((enum sunxi_mmc_kind)kind,(kind==SUNXI_MMC_SD?0x04020000:0x04022000)+0x200,1));
    puts("PASS: bounded CMD24 writes, immutable input, programming status, partial faults, default denial and one affected read");
    return 0;
}
