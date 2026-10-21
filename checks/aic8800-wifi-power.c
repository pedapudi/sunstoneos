/* Drive the actual Wi-Fi worker through management startup with each power
 * mode and compare its ME_CONFIG parameters with source-compiled structures. */
#include "aic8800_wifi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t expected[3][112];
static const int owner;
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
static struct aic8800_wifi_config config(unsigned mode)
{
    return (struct aic8800_wifi_config){.rf_profile_record=1,.channel_policy_record=2,.command_timeout=100,
        .profile=1,.channel_count=1,.channels={{2412,0,1,17}},.power_save=(uint8_t)mode};
}
/* Returns the configured power mode after interface creation. */
static unsigned start(unsigned mode,uint32_t features)
{
    static struct aic8800_wifi w;memset(&w,0,sizeof(w));
    struct aic8800_wifi_config c=config(mode);
    assert(aic8800_wifi_init(&w,&c)&&aic8800_wifi_claim(&w,&owner,1,100000));
    uint64_t now=1;unsigned configured=0;
    for(unsigned i=0;i<16&&w.state!=AIC8800_WIFI_READY;++i){
        aic8800_wifi_step(&w,&owner,++now);assert(w.state==AIC8800_WIFI_REQUEST);
        uint16_t id=(uint16_t)(w.request[8]|w.request[9]<<8);
        if(id==0x1400){
            unsigned want=(features&4u)?mode:0;
            assert(!memcmp(w.request+16,expected[want],112));configured=1;
        }
        assert(aic8800_wifi_sent(&w,&owner)&&w.state==AIC8800_WIFI_WAITING);
        uint8_t p[28]={0};uint16_t n=0;
        if(id==0x69)n=16;
        else if(id==0x73){n=6;p[0]=2;p[5]=5;}
        else if(id==4){n=28;put32(p+20,features);p[24]=8;p[26]=4;}
        else if(id==6){n=2;p[1]=1;}
        struct aic8800_sdio_response r={.has_message=1,.message_id=(uint16_t)(id+1),.parameters=p,.parameter_bytes=n};
        assert(aic8800_wifi_response(&w,&owner,&r,++now)==AIC8800_CODEC_OK);
    }
    assert(w.state==AIC8800_WIFI_READY&&configured&&w.vif==1);
    return w.power_mode;
}
int main(int argc,char **argv)
{
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(expected,112,3,f)==3&&fgetc(f)==EOF&&!fclose(f));
    /* Feature bit 8 (full MAC) is always present; bit 2 reports power save. */
    assert(start(AIC8800_WIFI_POWER_OFF,0x104)==AIC8800_WIFI_POWER_OFF);
    assert(start(AIC8800_WIFI_POWER_ON,0x104)==AIC8800_WIFI_POWER_ON);
    assert(start(AIC8800_WIFI_POWER_DYNAMIC,0x104)==AIC8800_WIFI_POWER_DYNAMIC);
    /* Without the feature the source clears ps_on; the request is not claimed. */
    assert(start(AIC8800_WIFI_POWER_ON,0x100)==AIC8800_WIFI_POWER_OFF);
    assert(start(AIC8800_WIFI_POWER_DYNAMIC,0x100)==AIC8800_WIFI_POWER_OFF);
    static struct aic8800_wifi w;
    struct aic8800_wifi_config c=config(3);assert(!aic8800_wifi_init(&w,&c));
    c=config(1);c.reserved[2]=1;assert(!aic8800_wifi_init(&w,&c)&&!w.initialized);
    puts("AIC Wi-Fi power save: source ME_CONFIG ps_on/dpsm for each mode, feature gating and refused profiles passed");
    return 0;
}
