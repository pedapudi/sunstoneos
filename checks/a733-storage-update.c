/* Actual storage service/platform/PMIC/pin/rail/clock composition. This fixture
 * models only MMIO, card media, monotonic time and the host transport. */
#include "a733_storage_update.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int update_read(void *,uintptr_t,uint32_t *);
static int update_write(void *,uintptr_t,uint32_t);
#define main inherited_platform_cases_not_run
#include "storage-provider-model.h"
#undef main
static struct a733_storage_update service;
static struct a733_storage_channel channel;
static uint8_t media[1024];
static unsigned issued,fifo_writes,uncertain,loss_during_write;
static int update_read(void *v,uintptr_t address,uint32_t *out)
{
    struct storage_model *s=v;struct model *c=&s->card;
    if(address==s->platform.base+0x200&&((c->data_command&63)==17||(c->data_command&63)==18)) {
        accessible(s);++s->accesses;++c->calls;assert(++c->fifo_this_poll<=16);
        uint64_t at=(uint64_t)c->reg[0x1c/4]*512+c->fifo_pos;
        assert(at>=1024&&at+4<=2048&&c->fifo_pos+4<=c->fifo_total);
        *out=0;for(unsigned i=0;i<4;++i)*out|=(uint32_t)media[at-1024+i]<<(8*i);
        c->fifo_pos+=4;return 1;
    }
    return storage_read(v,address,out);
}
static int update_write(void *v,uintptr_t address,uint32_t value)
{
    struct storage_model *s=v;struct model *c=&s->card;
    if(address==s->platform.base+0x200) {
        accessible(s);assert(a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,&service,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,NULL,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(SUNXI_MMC_EMMC,A733_MMC_EMMC_SDC3,&service,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,&service,address+0x1000,value));
        ++s->accesses;++c->calls;++fifo_writes;assert(++c->fifo_this_poll<=16);
        uint64_t at=(uint64_t)c->reg[0x1c/4]*512+c->fifo_pos;
        assert(at>=1024&&at+4<=2048&&c->fifo_pos+4<=512);
        for(unsigned i=0;i<4;++i)media[at-1024+i]=(uint8_t)(value>>(8*i));
        c->fifo_pos+=4;
        if(loss_during_write&&fifo_writes==1)a733_storage_channel_lost(&channel);
        return !(uncertain&&fifo_writes==1);
    }
    if(address==s->platform.base+0x18&&(value&63)==24) {
        accessible(s);assert(a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,&service,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,NULL,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(SUNXI_MMC_EMMC,A733_MMC_EMMC_SDC3,&service,address,value));
        assert(!a733_mmc_platform_native_transfer_allowed(s->platform.config.kind,s->platform.config.emmc_host,&service,address+0x1000,value));
        ++s->accesses;++c->calls;++issued;assert(c->selected&&c->reg[0x14/4]==512);
        c->reg[0x18/4]=value&~(1u<<31);c->reg[0x38/4]=4;c->reg[0x20/4]=0x900;
        c->data_command=value;c->fifo_pos=0;c->fifo_total=512;return 1;
    }
    return storage_write(v,address,value);
}
static void progress(void)
{
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
    assert(argc==5);uint8_t raw[256];load_file(argv[1],raw,sizeof(raw));load_file(argv[2],media,sizeof(media));
    struct a733_storage_grant g;assert(a733_storage_grant_decode(&g,raw,sizeof(raw)));
    storage_setup_selected((enum sunxi_mmc_kind)g.kind,(enum a733_mmc_emmc_host)g.host);
    field(m.card.csd,84,12,21);field(m.card.csd,22,4,9);ready_storage();
    a733_storage_channel_init(&channel);
    unsigned before=m.accesses;
    int initialized=a733_storage_update_init(&service,&m.platform,&g,1000000);
    if(!strcmp(argv[4],"wrong-card")){assert(!initialized&&!issued);save_file(argv[3],media,sizeof(media));return 0;}
    assert(initialized&&m.accesses>before&&m.platform.update==&service);
    struct sunxi_mmc_card blocked;
    before=m.accesses;
    assert(a733_mmc_platform_card(&m.platform,&blocked,1000000)==A733_MMC_INVALID&&m.accesses==before);
    assert(!a733_storage_update_mmio(&service,m.platform.base+0x200,1));
    uint8_t refused[88]={0};size_t no_response=0;uint8_t ignored[1152];
    m.power.worker=1;before=m.accesses;
    assert(!a733_storage_update_request(&service,refused,sizeof(refused)));
    a733_storage_update_poll(&service);
    assert(!a733_storage_update_response(&service,ignored,sizeof(ignored),&no_response));
    assert(!service.sequence&&service.state==A733_STORAGE_IDLE&&!atomic_load(&service.lost)&&m.accesses==before);
    m.power.worker=0;
    uncertain=!strcmp(argv[4],"uncertain");loss_during_write=!strcmp(argv[4],"loss");
    for(;;) {
        uint32_t n;size_t got=fread(&n,1,4,stdin);if(!got)break;assert(got==4&&n<=A733_STORAGE_BODY);
        uint8_t request[A733_STORAGE_BODY],response[A733_STORAGE_BODY];assert(fread(request,1,n,stdin)==n);
        size_t bytes=0;
        uint32_t operation;memcpy(&operation,request+36,4);
        if(!service.sequence&&operation==A733_STORAGE_INFO) {
            /* Use the host's existing INFO response; no extra card lifecycle. */
            assert(a733_storage_update_request(&service,request,n)&&service.response_ready);
            before=m.accesses;
            assert(!a733_storage_update_response(&service,(void *)&m.power.words[64],1152,&no_response));
            assert(!a733_storage_update_response(&service,response,sizeof(response),(void *)&m.power.words[64]));
            assert(service.response_ready&&m.accesses==before);
            assert(a733_storage_update_response(&service,response,sizeof(response),&bytes)&&!service.response_ready);
            n=(uint32_t)bytes;assert(fwrite(&n,1,4,stdout)==4&&fwrite(response,1,bytes,stdout)==bytes&&!fflush(stdout));
            continue;
        }
        assert(a733_storage_channel_submit(&channel,request,n));
        for(unsigned i=0;i<10000&&!atomic_load(&channel.lost);++i) {
            progress();if(a733_storage_channel_response(&channel,response,sizeof(response),&bytes))break;
        }
        if(atomic_load(&channel.lost)) {
            unsigned accesses=m.accesses;progress();assert(service.state==A733_STORAGE_RETAINED&&accesses==m.accesses);
            assert(m.platform.update==&service&&m.platform.clock.id&&m.platform.rail.id&&m.platform.pin.id);
            if(issued)assert(service.input==m.platform.core.borrowed&&m.platform.core.write_request.source==service.input);
            if(loss_during_write)assert(fifo_writes==1);
            n=0;assert(fwrite(&n,1,4,stdout)==4&&!fflush(stdout));break;
        }
        assert(bytes);n=(uint32_t)bytes;assert(fwrite(&n,1,4,stdout)==4&&fwrite(response,1,bytes,stdout)==bytes&&!fflush(stdout));
    }
    save_file(argv[3],media,sizeof(media));fprintf(stderr,"commands=%u fifo_words=%u retained=%u\n",issued,fifo_writes,service.state==A733_STORAGE_RETAINED);
    return 0;
}
