/* Native normal-mode D80 management and passive scan. Published ABI/sequence
 * references and explicit RF/channel admission are in docs/radio-wifi.md. */
#include "aic8800_wifi.h"
enum { POWER, CALIBRATE, MAC, RESET, VERSION, ME_CONFIG, CHANNELS, START, ADD_IF, SCAN };
static const uint16_t ids[]={0x77,0x69,0x73,0,4,0x1400,0x1402,2,6,0x1000};
static const uint16_t sizes[]={69,24,4,0,0,112,254,72,10,376};
static int span(const void *p,size_t n){return p&&n<=UINTPTR_MAX-(uintptr_t)p;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{return (uintptr_t)a<(uintptr_t)b+bn&&(uintptr_t)b<(uintptr_t)a+an;}
static uint16_t le16(const uint8_t *p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(v>>(8*i));}
void aic8800_wifi_stop(struct aic8800_wifi *w,const void *owner,enum aic8800_wifi_error e)
{
    if(!w||!w->initialized||w->owner!=owner||w->state==AIC8800_WIFI_FAULT)return;
    w->error=e;w->state=AIC8800_WIFI_FAULT;
}
void aic8800_wifi_observe(struct aic8800_wifi *w,const void *owner,uint64_t now)
{
    if(!w||!w->initialized||w->owner!=owner||w->state==AIC8800_WIFI_FAULT)return;
    int active=w->state==AIC8800_WIFI_PREPARING||w->state==AIC8800_WIFI_REQUEST||w->state==AIC8800_WIFI_WAITING;
    uint64_t limit=active?w->deadline:0;
    if(active&&w->command_deadline&&w->command_deadline<limit)limit=w->command_deadline;
    if(w->completion_deadline&&(!limit||w->completion_deadline<limit))limit=w->completion_deadline;
    if(now<w->last_now||(limit&&now>=limit))aic8800_wifi_stop(w,owner,AIC8800_WIFI_TIME_ERROR);
    else {w->last_now=now;w->completion_deadline=0;}
}
int aic8800_wifi_init(struct aic8800_wifi *w,const struct aic8800_wifi_config *c)
{
    if(!span(w,sizeof(*w))||!span(c,sizeof(*c))||overlap(w,sizeof(*w),c,sizeof(*c))||w->initialized||
        c->profile!=1||!c->rf_profile_record||!c->channel_policy_record||!c->command_timeout||
        !c->channel_count||c->channel_count>AIC8800_WIFI_CHANNELS)return 0;
    unsigned bands[2]={0};
    for(unsigned i=0;i<c->channel_count;++i) {
        const struct aic8800_wifi_channel *v=&c->channels[i];
        if(v->band>1||(v->flags!=1&&v->flags!=5)||
            (!v->band&&!((v->frequency>=2412&&v->frequency<=2472&&(v->frequency-2412)%5==0)||v->frequency==2484))||
            (v->band&&(v->frequency<5000||v->frequency>5900||v->frequency%5)))return 0;
        if(++bands[v->band]>(v->band?28u:14u))return 0;
        for(unsigned j=0;j<i;++j)if(c->channels[j].frequency==v->frequency)return 0;
    }
    struct aic8800_wifi initial={0};initial.config=*c;initial.initialized=1;*w=initial;return 1;
}
int aic8800_wifi_claim(struct aic8800_wifi *w,const void *owner,uint64_t now,uint64_t deadline)
{
    if(!w||!w->initialized||w->owner||!owner||w->state!=AIC8800_WIFI_NEW||deadline<=now)return 0;
    w->owner=owner;w->last_now=now;w->deadline=deadline;w->state=AIC8800_WIFI_PREPARING;return 1;
}
int aic8800_wifi_scan(struct aic8800_wifi *w,const void *owner,uint64_t now,uint64_t deadline)
{
    if(!w||!w->initialized||w->owner!=owner||w->state!=AIC8800_WIFI_READY||w->held||
        now<w->last_now||deadline<=now)return 0;
    w->last_now=now;w->deadline=deadline;w->command_deadline=0;w->phase=SCAN;w->scan_done=0;
    w->summary=(struct aic8800_wifi_scan_summary){.vif=w->vif};w->state=AIC8800_WIFI_PREPARING;return 1;
}
static void channel(uint8_t *p,const struct aic8800_wifi_channel *c)
{put16(p,c->frequency);p[2]=c->band;p[3]=c->flags;p[4]=(uint8_t)c->max_power_dbm;}
static uint8_t crc(const uint8_t *p)
{
    uint8_t c=0;for(unsigned i=0;i<3;++i){c^=p[i];for(unsigned j=0;j<8;++j)c=(uint8_t)((c<<1)^((c&128)?7:0));}return c;
}
void aic8800_wifi_step(struct aic8800_wifi *w,const void *owner,uint64_t now)
{
    if(!w||!w->initialized||w->owner!=owner)return;
    aic8800_wifi_observe(w,owner,now);if(w->state!=AIC8800_WIFI_PREPARING)return;
    if(w->phase>SCAN||w->config.command_timeout>UINT64_MAX-now){aic8800_wifi_stop(w,owner,AIC8800_WIFI_TIME_ERROR);return;}
    for(unsigned i=0;i<512;++i)w->request[i]=0;
    uint16_t id=ids[w->phase],size=sizes[w->phase];uint8_t *p=w->request+16;
    put16(w->request,(uint16_t)(size+12));w->request[2]=0x11;w->request[3]=crc(w->request);
    put16(w->request+8,id);put16(w->request+10,id>>10);put16(w->request+12,100);put16(w->request+14,size);
    switch(w->phase) {
    case POWER: {
        static const uint8_t power[69]={1,
            20,20,20,20,20,20,20,20,18,18,16,16,
            20,20,20,20,18,18,16,16,16,16,
            20,20,20,20,18,18,16,16,16,16,15,15,
            128,128,128,128,20,20,20,20,18,18,16,16,
            20,20,20,20,18,18,16,16,16,15,
            20,20,20,20,18,18,16,16,16,15,14,14};
        for(unsigned i=0;i<69;++i)p[i]=power[i];
        break;
    }
    case CALIBRATE:put32(p,0x0f8f);put32(p+4,0x0f0f);put32(p+8,0x0c34c008);put32(p+16,0x264203);break;
    case MAC:put32(p,1);break;
    case ME_CONFIG:
        /* Explicit legacy management-only profile: 20MHz, no HT/VHT/HE/PS.
         * Source sender always includes LDPC in the unused HT field. */
        put16(p,1);put16(p+100,512);break;
    case CHANNELS: {
        unsigned counts[2]={0};
        for(unsigned i=0;i<w->config.channel_count;++i) {
            const struct aic8800_wifi_channel *c=&w->config.channels[i];
            channel(p+(c->band?84:0)+6*counts[c->band]++,c);
        }
        p[252]=(uint8_t)counts[0];p[253]=(uint8_t)counts[1];break;
    }
    case START:put32(p+64,300);put16(p+68,20);break;
    case ADD_IF:for(unsigned i=0;i<6;++i)p[2+i]=w->mac[i];break;
    case SCAN:
        for(unsigned i=0;i<w->config.channel_count;++i)channel(p+i*6,&w->config.channels[i]);
        for(unsigned i=0;i<6;++i)p[352+i]=255;
        p[366]=w->vif;p[367]=(uint8_t)w->config.channel_count;
        /* NO_IR is mandatory on every channel. Zero SSIDs/IEs are additional
         * source passive-profile conditions, not a substitute for that flag. */
        break;
    default:break;
    }
    w->expected_cfm=w->phase==SCAN?0x1009:(uint16_t)(id+1);
    w->command_deadline=now+w->config.command_timeout;
    if(w->command_deadline>w->deadline)w->command_deadline=w->deadline;
    w->state=AIC8800_WIFI_REQUEST;
}
int aic8800_wifi_sent(struct aic8800_wifi *w,const void *owner)
{
    if(!w||!w->initialized||w->owner!=owner||w->state!=AIC8800_WIFI_REQUEST)return 0;
    w->state=AIC8800_WIFI_WAITING;return 1;
}
static int scan_result(struct aic8800_wifi *w,const struct aic8800_sdio_response *r)
{
    const uint8_t *p=r->parameters;size_t n=r->parameter_bytes;
    if(n<12)return -1;
    if(p[8]!=w->vif&&p[8]!=255)return 0;
    uint16_t length=le16(p),freq=le16(p+4);int found=0;
    for(unsigned i=0;i<w->config.channel_count;++i)
        if(w->config.channels[i].frequency==freq&&w->config.channels[i].band==p[6])found=1;
    if(!found||length<36||length>AIC8800_WIFI_FRAME_BYTES||length>n-12)return -1;
    const uint8_t *frame=p+12;uint16_t fc=le16(frame);
    if((fc&0x00fc)!=0x0080&&(fc&0x00fc)!=0x0050)return -1;
    struct aic8800_wifi_scan_result result={.frequency=freq,.frame_control=le16(p+2),.frame_bytes=length,
        .rssi_dbm=(int8_t)p[9],.band=p[6],.vif=p[8],.frame=w->frame};
    for(unsigned i=0;i<6;++i)result.bssid[i]=frame[16+i];
    size_t at=36;
    while(at<length) {
        if(length-at<2||frame[at+1]>length-at-2)return -1;
        if(frame[at]==0) {
            if(result.has_ssid||frame[at+1]>32)return -1;
            result.has_ssid=1;result.ssid_bytes=frame[at+1];
            for(unsigned i=0;i<result.ssid_bytes;++i)result.ssid[i]=frame[at+2+i];
        }
        at+=2+frame[at+1];
    }
    if(w->held||w->summary.received==UINT32_MAX)return -1;
    for(unsigned i=0;i<length;++i)w->frame[i]=frame[i];
    w->result=result;w->held=1;w->held_record=r->record;++w->summary.received;return 1;
}
static void completed(struct aic8800_wifi *w)
{
    /* Retain the active bound through the transport's post-callback observation,
     * even when this response changes state or clears the command deadline. */
    w->completion_deadline=w->deadline;
    if(w->command_deadline&&w->command_deadline<w->completion_deadline)
        w->completion_deadline=w->command_deadline;
}
static void finish_scan(struct aic8800_wifi *w)
{if(w->summary.accepted&&w->scan_done){w->state=AIC8800_WIFI_SCAN_DONE;w->command_deadline=0;}}
enum aic8800_codec_result aic8800_wifi_response(struct aic8800_wifi *w,const void *owner,
    const struct aic8800_sdio_response *r,uint64_t now)
{
    if(!w||!w->initialized||w->owner!=owner||!r)return AIC8800_CODEC_INVALID;
    aic8800_wifi_observe(w,owner,now);if(w->state==AIC8800_WIFI_FAULT)return AIC8800_CODEC_INVALID;
    if(!r->has_message||w->state!=AIC8800_WIFI_WAITING)return AIC8800_CODEC_UNRELATED;
    if(w->phase==SCAN&&r->message_id==0x1004) {
        if(scan_result(w,r)<0)aic8800_wifi_stop(w,owner,AIC8800_WIFI_RESULT_ERROR);
        return AIC8800_CODEC_UNRELATED;
    }
    if(w->phase==SCAN&&r->message_id==0x1001) {
        if(r->parameter_bytes!=3){aic8800_wifi_stop(w,owner,AIC8800_WIFI_MESSAGE_ERROR);return AIC8800_CODEC_MALFORMED;}
        if(r->parameters[0]!=w->vif||w->scan_done)return AIC8800_CODEC_UNRELATED;
        completed(w);w->summary.status=r->parameters[1];w->summary.reported_results=r->parameters[2];w->scan_done=1;
        if(w->summary.status)aic8800_wifi_stop(w,owner,AIC8800_WIFI_STATUS_ERROR);else finish_scan(w);
        return AIC8800_CODEC_OK;
    }
    if(r->message_id!=w->expected_cfm||(w->phase==SCAN&&w->summary.accepted))return AIC8800_CODEC_UNRELATED;
    unsigned required=0;
    if(w->phase==CALIBRATE)required=16;else if(w->phase==MAC)required=6;
    else if(w->phase==VERSION)required=28;else if(w->phase==ADD_IF)required=2;
    if((required&&r->parameter_bytes!=required)||r->parameter_bytes>sizeof(w->last_cfm)) {
        aic8800_wifi_stop(w,owner,AIC8800_WIFI_MESSAGE_ERROR);return AIC8800_CODEC_MALFORMED;
    }
    completed(w);w->last_cfm_id=r->message_id;w->last_cfm_bytes=r->parameter_bytes;
    for(unsigned i=0;i<r->parameter_bytes;++i)w->last_cfm[i]=r->parameters[i];
    const uint8_t *p=r->parameters;
    if(w->phase==CALIBRATE)for(unsigned i=0;i<4;++i)w->calibration_addresses[i]=le32(p+4*i);
    else if(w->phase==MAC) {
        unsigned any=0;for(unsigned i=0;i<6;++i){w->mac[i]=p[i];any|=p[i];}
        if(!any||(p[0]&1)){aic8800_wifi_stop(w,owner,AIC8800_WIFI_MAC_ERROR);return AIC8800_CODEC_OK;}
    } else if(w->phase==VERSION) {
        for(unsigned i=0;i<6;++i)w->version[i]=le32(p+4*i);
        w->max_stations=le16(p+24);w->max_vifs=p[26];
        if(!(w->version[5]&(1u<<8))||!w->max_vifs){aic8800_wifi_stop(w,owner,AIC8800_WIFI_VERSION_ERROR);return AIC8800_CODEC_OK;}
    } else if(w->phase==ADD_IF) {
        w->vif=p[1];
        if(p[0]||w->vif>=w->max_vifs){aic8800_wifi_stop(w,owner,AIC8800_WIFI_STATUS_ERROR);return AIC8800_CODEC_OK;}
        w->state=AIC8800_WIFI_READY;w->command_deadline=0;return AIC8800_CODEC_OK;
    } else if(w->phase==SCAN) {
        w->summary.accepted=1;w->command_deadline=0;finish_scan(w);return AIC8800_CODEC_OK;
    }
    ++w->phase;w->command_deadline=0;w->state=AIC8800_WIFI_PREPARING;return AIC8800_CODEC_OK;
}
void aic8800_wifi_consumed(struct aic8800_wifi *w,const void *owner,const uint8_t *record)
{
    if(w&&w->owner==owner&&w->held&&w->held_record==record&&w->state!=AIC8800_WIFI_FAULT) {
        w->held=0;w->held_record=NULL;
    }
}
