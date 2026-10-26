/* Actual storage service/platform/PMIC/pin/rail/clock composition. This fixture
 * models only MMIO, card media, monotonic time and the host transport. */
#include "a733_storage_update.h"
#include "a733_mmc_read.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int update_read(void *,uintptr_t,uint32_t *);
static int update_write(void *,uintptr_t,uint32_t);
static inline int update_write_fifo(void *,uintptr_t,const uint32_t *,unsigned);
#define main inherited_platform_cases_not_run
#include "storage-provider-model.h"
#undef main
static struct a733_storage_update service;
static struct a733_storage_channel channel;
/* Card media holds exactly the granted range, sectors 2-3 by default. Checks that
 * reuse this model without main() fill the fixed array directly; main points
 * card_media at a larger buffer when a grant covers another range. */
static uint8_t media[1024];
static uint8_t *card_media=media;static uint64_t media_base=1024,media_bytes=sizeof(media);
static unsigned issued,fifo_writes,uncertain,loss_during_write;
/* Card programming time after each CMD24; CMD13 reports PRG until it ends. */
static uint64_t program_us,programmed_until;
static unsigned bursting,programming_polls;
/* Mode "hs" runs the session at SD profile two. The card then also answers
 * ACMD51, ACMD6 and CMD6 after selection, as in checks/sunxi-mmc-sd-modes.c,
 * and the controller runs at the module rate the platform programmed. */
static unsigned sd_profile,sd_width;
static uint8_t sd_scr[8]={2,5},sd_switch[64];
/* This model adds only the maintenance composition's DMA path; descriptor
 * completion/reset/error qualification belongs to storage-read-idmac.py. */
static _Alignas(64) struct sunxi_mmc_dma_descriptor descriptors[SUNXI_MMC_DMA_DESCRIPTORS];
static unsigned dma_enabled,dma_pending,dma_completed,dma_reads,dma_before,dma_after,dma_failure;
static uint64_t dma_first,write_deadline;
static unsigned dma_bytes;
static int update_sync(void *v,uint8_t *buffer,size_t bytes,unsigned phase)
{
    assert(v==&m&&buffer==service.readback&&bytes&&bytes<=sizeof(service.readback)&&!(bytes%512));
    assert(m.platform.core.borrowed==buffer&&m.platform.core.borrowed_bytes==bytes);
    if(phase==SUNXI_MMC_DMA_BEFORE) {
        assert(!dma_pending&&!m.card.reg[0x80/4]);++dma_before;
        assert(a733_mmc_dma_chain(descriptors,buffer,bytes,(uint32_t)bytes));
        if(dma_failure==1)return 0;
    } else {
        assert(phase==SUNXI_MMC_DMA_AFTER&&dma_pending&&dma_completed);
        assert(!m.card.reg[0x80/4]&&!(m.card.reg[0]&0x27)&&!(m.card.reg[0x88/4]&0x1e000));
        for(unsigned i=0;i<(bytes+4095)/4096;++i)assert(!(descriptors[i].config&(1u<<31)));
        if(dma_failure==2)return 0;
        assert(dma_first>=media_base&&bytes<=media_bytes-(dma_first-media_base));
        memcpy(buffer,card_media+dma_first-media_base,bytes);++dma_after;dma_pending=0;
    }
    return 1;
}
static void reachable(struct storage_model *s)
{
    if(!sd_profile){accessible(s);return;}
    assert(s->admitted&&s->power.register_held&&s->platform.pin.id&&!s->power.pins.failed);
    assert(a733_rail_status(&s->power.rails,&s->platform.rail)==A733_RAIL_READY);
    assert((*pmic_model_word(&s->power,s->platform.module)&0x87001f1f)==s->platform.running_module);
    assert((*pmic_model_word(&s->power,s->platform.bus)&0x10001)==0x10001);
}
static int controller(struct storage_model *s,uintptr_t address)
{
    return sd_profile&&address>=s->platform.base&&address<=s->platform.base+0x300&&
        s->power.register_held&&a733_mmc_platform_native_address_selected(s->platform.config.kind,s->platform.config.emmc_host,address,0);
}
static int mode_read(struct storage_model *s,uintptr_t address,uint32_t *out)
{
    struct model *c=&s->card;unsigned command=c->data_command&63;
    ++s->accesses;reachable(s);
    if(address==s->platform.base+0x200&&(command==51||command==6)) {
        assert(++c->fifo_this_poll<=SUNXI_MMC_FIFO_WORDS&&c->fifo_pos<c->fifo_total);*out=0;
        for(unsigned b=0;b<4;++b,++c->fifo_pos)*out|=(uint32_t)(command==51?sd_scr[c->fifo_pos]:sd_switch[c->fifo_pos])<<(8*b);
        return 1;
    }
    return read32(c,(uint32_t)(address-s->platform.base),out);
}
static int mode_write(struct storage_model *s,uintptr_t address,uint32_t value)
{
    struct model *c=&s->card;unsigned command=value&63;uint32_t argument=c->reg[0x1c/4];
    ++s->accesses;++s->writes;reachable(s);
    if(address!=s->platform.base+0x18||(value&(1u<<21))||!c->selected||(command!=55&&command!=51&&command!=6))
        return write32(c,(uint32_t)(address-s->platform.base),value);
    assert(!(value&(1u<<10)));c->commands[c->command_count]=command;c->args[c->command_count++]=argument;
    c->reg[0x18/4]=value&~(1u<<31);c->reg[0x38/4]=4;c->reg[0x20/4]=0x900;c->data_command=0;c->fifo_total=c->fifo_pos=0;
    if(command==55){c->app=1;c->reg[0x20/4]|=1u<<5;}
    else if(command==51){assert(c->app&&!argument&&c->reg[0x10/4]==8);c->app=0;c->data_command=value;c->fifo_total=8;}
    else if(!(value&(1u<<9))){assert(c->app&&argument==2);c->app=0;sd_width=4;}
    else {
        assert(!c->app&&sd_width==4&&c->reg[0xc/4]==1&&c->reg[0x10/4]==64);
        assert(argument==0x00fffff1||argument==0x80fffff1);c->data_command=value;c->fifo_total=64;
        memset(sd_switch,0,sizeof(sd_switch));sd_switch[13]=2;if(argument>>31)sd_switch[16]=1;
    }
    return 1;
}
static int update_read(void *v,uintptr_t address,uint32_t *out)
{
    struct storage_model *s=v;struct model *c=&s->card;
    if(dma_pending&&address==s->platform.base+0x38&&!dma_completed) {
        dma_completed=1;c->fifo_pos=c->fifo_total;
        c->reg[0x38/4]|=8|((c->data_command&63)==18?1u<<14:0);c->reg[0x88/4]=2;
        for(unsigned i=0;i<(dma_bytes+4095)/4096;++i)descriptors[i].config&=~(1u<<31);
        if(dma_failure==3)a733_storage_channel_lost(&channel);
        if(dma_failure==4)m.power.now+=1000000;
    }
    if(address==s->platform.base+0x200&&((c->data_command&63)==17||(c->data_command&63)==18)) {
        reachable(s);++s->accesses;++c->calls;assert(++c->fifo_this_poll<=SUNXI_MMC_FIFO_WORDS);
        uint64_t at=(uint64_t)c->reg[0x1c/4]*512+c->fifo_pos;
        assert(at>=media_base&&at+4<=media_base+media_bytes&&c->fifo_pos+4<=c->fifo_total);
        *out=0;for(unsigned i=0;i<4;++i)*out|=(uint32_t)card_media[at-media_base+i]<<(8*i);
        c->fifo_pos+=4;return 1;
    }
    if(controller(s,address))return mode_read(s,address,out);
    return storage_read(v,address,out);
}
static int update_write(void *v,uintptr_t address,uint32_t value)
{
    struct storage_model *s=v;struct model *c=&s->card;
    if(dma_enabled&&address==s->platform.base+0x80) {
        reachable(s);c->reg[0x80/4]=value==1?0:value;return 1;
    }
    if(dma_enabled&&address==s->platform.base+0x88) {
        reachable(s);c->reg[0x88/4]&=~value;return 1;
    }
    if(dma_enabled&&address==s->platform.base+0x18&&(c->reg[0]&32)&&(value&(1u<<9))&&!(value&(1u<<10))) {
        reachable(s);assert((value&63)==17||(value&63)==18);
        assert((c->reg[0]&(1u<<5))&&!(c->reg[0]&(1u<<31))&&c->reg[0x80/4]==0x82);
        assert(c->reg[0x84/4]==(uint32_t)((uintptr_t)descriptors>>2));
        dma_bytes=c->reg[0x14/4];dma_first=(uint64_t)c->reg[0x1c/4]*512;
        assert(a733_mmc_dma_chain(descriptors,service.readback,dma_bytes,dma_bytes));
        if(service.state==A733_STORAGE_VERIFY_WAIT)assert(write_deadline==service.operation_deadline);
        assert(dma_before==dma_reads+1&&!dma_pending);++dma_reads;dma_pending=1;dma_completed=0;
        c->reg[0x18/4]=value&~(1u<<31);c->reg[0x38/4]=4;c->reg[0x20/4]=0x900;
        c->reg[0x24/4]=0x900;c->data_command=value;c->fifo_pos=0;c->fifo_total=dma_bytes;return 1;
    }
    if(address==s->platform.base+0x200) {
        reachable(s);
        if(!bursting) {
            assert(a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,&service,address,value));
            assert(!a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,NULL,address,value));
            assert(!a733_mmc_platform_native_transfer_allowed(SUNXI_MMC_EMMC,A733_MMC_EMMC_SDC3,&service,address,value));
            assert(!a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,&service,address+0x1000,value));
        }
        ++s->accesses;++c->calls;++fifo_writes;assert(++c->fifo_this_poll<=SUNXI_MMC_FIFO_WORDS);
        uint64_t at=(uint64_t)c->reg[0x1c/4]*512+c->fifo_pos;
        assert(at>=media_base&&at+4<=media_base+media_bytes&&c->fifo_pos+4<=512);
        for(unsigned i=0;i<4;++i)card_media[at-media_base+i]=(uint8_t)(value>>(8*i));
        c->fifo_pos+=4;
        if(loss_during_write&&fifo_writes==1)a733_storage_channel_lost(&channel);
        return !(uncertain&&fifo_writes==1);
    }
    if(address==s->platform.base+0x18&&(value&63)==24) {
        reachable(s);assert(a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,&service,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,NULL,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(SUNXI_MMC_EMMC,A733_MMC_EMMC_SDC3,&service,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,&service,address+0x1000,value));
        ++s->accesses;++c->calls;++issued;assert(c->selected&&c->reg[0x14/4]==512);
        assert(!(c->reg[0]&32)&&(c->reg[0]&(1u<<31))&&!c->reg[0x80/4]&&!dma_pending);
        write_deadline=service.operation_deadline;
        if(sd_profile)assert(sd_width==4&&c->reg[0xc/4]==1&&s->platform.running_module==0x81000003u);
        c->reg[0x18/4]=value&~(1u<<31);c->reg[0x38/4]=4;c->reg[0x20/4]=0x900;
        c->data_command=value;c->fifo_pos=0;c->fifo_total=512;programmed_until=m.power.now+program_us;return 1;
    }
    if(address==s->platform.base+0x18&&(value&63)==13&&!(value&(1u<<21))&&issued&&m.power.now<programmed_until) {
        /* CMD13 completes with PRG state; repeated polls stay out of the
         * card model's bounded command log. */
        reachable(s);++s->accesses;++c->calls;++programming_polls;assert(c->selected);
        c->reg[0x18/4]=value&~(1u<<31);c->reg[0x38/4]=4;c->reg[0x20/4]=7u<<9;
        c->data_command=0;c->fifo_pos=c->fifo_total=0;return 1;
    }
    if(controller(s,address))return mode_write(s,address,value);
    return storage_write(v,address,value);
}
/* The native adapter admits a FIFO burst once, then stores every word. */
static inline int update_write_fifo(void *v,uintptr_t address,const uint32_t *words,unsigned n)
{
    struct storage_model *s=v;struct model *c=&s->card;
    enum sunxi_mmc_kind kind=s->platform.config.kind;enum a733_mmc_emmc_host host=s->platform.config.emmc_host;
    assert(a733_mmc_platform_native_fifo_allowed(kind,host,&service,address,n));
    if(n==(c->fifo_total-c->fifo_pos)/4)assert(!a733_mmc_platform_native_fifo_allowed(kind,host,&service,address,n+1));
    assert(!a733_mmc_platform_native_fifo_allowed(kind,host,NULL,address,n));
    assert(!a733_mmc_platform_native_fifo_allowed(SUNXI_MMC_EMMC,A733_MMC_EMMC_SDC3,&service,address,n));
    assert(!a733_mmc_platform_native_fifo_allowed(kind,host,&service,address+0x1000,n));
    bursting=1;
    for(unsigned i=0;i<n;++i)if(update_write(v,address,words[i])!=1){bursting=0;return 0;}
    bursting=0;return 1;
}
static void progress(void)
{
    /* Nothing here reads the card model's 64-entry command log; restarting it
     * lets a session cover more than a few sectors. */
    if(m.card.command_count>32)m.card.command_count=0;
    m.power.now+=100;m.card.fifo_this_poll=0;
    enum a733_pmic_status r=pmic_model_step(&m.power);
    assert(r!=A733_PMIC_RETAINED||service.state==A733_STORAGE_RETAINED);
    a733_storage_channel_worker(&channel,&service);
}
static void load_file(const char *name,uint8_t *p,size_t n)
{FILE *f=fopen(name,"rb");assert(f&&fread(p,1,n,f)==n&&fgetc(f)==EOF);assert(!fclose(f));}
static void save_file(const char *name,const uint8_t *p,size_t n)
{FILE *f=fopen(name,"wb");assert(f&&fwrite(p,1,n,f)==n&&!fclose(f));}
int main(int argc,char **argv)
{
    if(argc==2&&!strcmp(argv[1],"--digest")) {
        uint8_t data[257],out[32];for(unsigned i=0;i<257;++i)data[i]=(uint8_t)i;
        const unsigned sizes[]={0,1,55,56,63,64,65,257};
        for(unsigned k=0;k<sizeof(sizes)/sizeof(*sizes);++k) {
            struct storage_digest d;storage_digest_init(&d);
            for(unsigned i=0;i<sizes[k];++i)assert(storage_digest_add(&d,data+i,1));
            storage_digest_end(&d,out);printf("%u ",sizes[k]);for(unsigned i=0;i<32;++i)printf("%02x",out[i]);puts("");
        }return 0;
    }
    assert(argc==5);uint8_t raw[256];load_file(argv[1],raw,sizeof(raw));
    struct a733_storage_grant g;assert(a733_storage_grant_decode(&g,raw,sizeof(raw)));
    assert(g.kind==SUNXI_MMC_SD&&g.blocks<=(1u<<20)&&g.sectors%1024==0&&g.sectors/1024<=(1u<<22));
    if(g.first*512!=media_base||g.blocks*512!=media_bytes) {
        media_base=g.first*512;media_bytes=g.blocks*512;card_media=malloc(media_bytes);assert(card_media);
    }
    load_file(argv[2],card_media,media_bytes);
    storage_setup_selected((enum sunxi_mmc_kind)g.kind,(enum a733_mmc_emmc_host)g.host);
    field(m.card.csd,84,12,21);field(m.card.csd,22,4,9);
    field(m.card.csd,48,22,(uint32_t)(g.sectors/1024-1));
    dma_enabled=!strncmp(argv[4],"dma",3);
    dma_failure=!strcmp(argv[4],"dma-before")?1:!strcmp(argv[4],"dma-after")?2:!strcmp(argv[4],"dma-loss")?3:!strcmp(argv[4],"dma-deadline")?4:0;
    sd_profile=(dma_enabled||!strcmp(argv[4],"hs"))?SUNXI_MMC_SD_HIGH_SPEED4:SUNXI_MMC_SD_LEGACY;
    if(sd_profile) {
        /* Switch class, 25 MHz TRAN_SPEED, PERI0 400 MHz output and PF0-5 routing. */
        field(m.card.csd,84,12,21|(1u<<10));field(m.card.csd,96,8,0x32);
        *pmic_model_word(&m.power,0x020020a0)=0xfe000000u|(99u<<8)|(1u<<20);
        *pmic_model_word(&m.power,0x02003908)=((1u<<11)|(1u<<2)|(1u<<1))*0x10001;
        *pmic_model_word(&m.power,0x02003988)=((1u<<11)|(1u<<2)|(1u<<1))<<16;
        struct a733_mmc_platform_config c=m.platform.config;c.sd_profile=SUNXI_MMC_SD_HIGH_SPEED4;
        for(unsigned i=0;i<7;++i)c.desired[i].function=i<6?2:0;
        struct a733_mmc_platform_ops ops=m.platform.ops;
        if(dma_enabled){c.dma=descriptors;ops.dma_sync=update_sync;}
        memset(&m.platform,0,sizeof(m.platform));
        assert(a733_mmc_platform_init(&m.platform,&c,&ops,&m,&m.power.clocks,&m.power.pins,&m.power.rails,&m.power.provider));
    }
    ready_storage();
    if(sd_profile)assert(m.platform.running_module==0x81000003u&&m.platform.core.card.bus_width==4&&
        m.platform.core.card.card_hz==50000000u&&(m.platform.clock.shared&A733_CLOCK_PERI0));
    a733_storage_channel_init(&channel);
    unsigned before=m.accesses;
    int initialized=a733_storage_update_init(&service,&m.platform,&g,1000000);
    if(!strcmp(argv[4],"wrong-card")){assert(!initialized&&!issued);save_file(argv[3],card_media,media_bytes);return 0;}
    assert(initialized&&m.accesses>before&&m.platform.update==&service);
    struct sunxi_mmc_card blocked;
    before=m.accesses;
    assert(a733_mmc_platform_card(&m.platform,&blocked,1000000)==A733_MMC_INVALID&&m.accesses==before);
    assert(!a733_storage_update_mmio(&service,m.platform.base+0x200,1));
    uint8_t refused[88]={0};size_t no_response=0;uint8_t ignored[A733_STORAGE_BODY];
    m.power.worker=1;before=m.accesses;
    assert(!a733_storage_update_request(&service,refused,sizeof(refused)));
    a733_storage_update_poll(&service);
    assert(!a733_storage_update_response(&service,ignored,sizeof(ignored),&no_response));
    assert(!service.sequence&&service.state==A733_STORAGE_IDLE&&!atomic_load(&service.lost)&&m.accesses==before);
    m.power.worker=0;
    uncertain=!strcmp(argv[4],"uncertain");loss_during_write=!strcmp(argv[4],"loss");
    /* SDXC permits 500 ms of write busy per block; "stalled" exceeds the
     * one-second request deadline that also covers the readback. */
    program_us=!strcmp(argv[4],"slow")?500000:!strcmp(argv[4],"stalled")?1000000:0;
    for(;;) {
        uint32_t n;size_t got=fread(&n,1,4,stdin);if(!got)break;assert(got==4&&n<=A733_STORAGE_BODY);
        uint8_t request[A733_STORAGE_BODY],response[A733_STORAGE_BODY];assert(fread(request,1,n,stdin)==n);
        size_t bytes=0;
        uint32_t operation;memcpy(&operation,request+36,4);
        if(!service.sequence&&operation==A733_STORAGE_INFO) {
            /* Use the host's existing INFO response; no extra card lifecycle. */
            assert(a733_storage_update_request(&service,request,n)&&service.response_ready);
            before=m.accesses;
            assert(!a733_storage_update_response(&service,(void *)&m.power.words[64],A733_STORAGE_BODY,&no_response));
            assert(!a733_storage_update_response(&service,response,sizeof(response),(void *)&m.power.words[64]));
            assert(service.response_ready&&m.accesses==before);
            assert(a733_storage_update_response(&service,response,sizeof(response),&bytes)&&!service.response_ready);
            n=(uint32_t)bytes;assert(fwrite(&n,1,4,stdout)==4&&fwrite(response,1,bytes,stdout)==bytes&&!fflush(stdout));
            continue;
        }
        assert(a733_storage_channel_submit(&channel,request,n));
        for(unsigned i=0;i<30000&&!atomic_load(&channel.lost);++i) {
            progress();if(a733_storage_channel_response(&channel,response,sizeof(response),&bytes))break;
        }
        if(atomic_load(&channel.lost)) {
            fprintf(stderr,"retained: core=%u error=%u step=%u dma=%u/%u/%u regs=%x/%x/%x\n",m.platform.core.state,m.platform.core.result.error,m.platform.core.command_step,dma_before,dma_reads,dma_after,m.card.reg[0],m.card.reg[0x38/4],m.card.reg[0x88/4]);
            unsigned accesses=m.accesses;progress();assert(service.state==A733_STORAGE_RETAINED&&accesses==m.accesses);
            assert(m.platform.update==&service&&m.platform.clock.id&&m.platform.rail.id&&m.platform.pin.id);
            if(dma_enabled)assert(service.readback==m.platform.core.borrowed);
            else if(issued)assert(service.input==m.platform.core.borrowed&&m.platform.core.write_request.source==service.input);
            if(loss_during_write)assert(fifo_writes==128); /* The admitted burst completes. */
            n=0;assert(fwrite(&n,1,4,stdout)==4&&!fflush(stdout));break;
        }
        assert(bytes);n=(uint32_t)bytes;assert(fwrite(&n,1,4,stdout)==4&&fwrite(response,1,bytes,stdout)==bytes&&!fflush(stdout));
    }
    if(dma_enabled&&!dma_failure)assert(dma_before==dma_reads&&dma_reads==dma_after&&!dma_pending);
    fprintf(stderr,"dma_reads=%u clean=%u invalidate=%u stride=%u\n",dma_reads,dma_before,dma_after,dma_enabled?64u:8u);
    save_file(argv[3],card_media,media_bytes);fprintf(stderr,"commands=%u fifo_words=%u programming_polls=%u retained=%u model_us=%llu\n",issued,fifo_writes,
        programming_polls,service.state==A733_STORAGE_RETAINED,(unsigned long long)m.power.now);
    return 0;
}
