/* Actual BSD RSN state/crypto + native key/data/transport/card/controller.
 * Firmware execution, association and decrypted RX are explicit model inputs. */
#define AIC8800_RUNTIME_FIXTURE_MAIN security_runtime_unused_main
#include "aic8800-runtime.c"
#undef AIC8800_RUNTIME_FIXTURE_MAIN
#include "aic8800_supplicant.h"
#include "aic8800_dispatch.h"
#define SUNSTONE_AIC8800_FAULT_SITE_CAPTURE 1
#include "aic8800_fault_site.h"
#ifdef SUNSTONE_AIC8800_SAE
/* OpenSSL allocates from the FreeRTOS heap in images. */
void *pvPortMalloc(size_t n){return malloc(n);}
void vPortFree(void *p){free(p);}
#endif
static struct aic8800_wifi wifi;
static struct aic8800_data data;
static struct aic8800_security security;
static struct aic8800_supplicant supplicant;
static uint8_t vectors[15][256];static unsigned sizes[15];
static unsigned entropy_calls, key_adds, deletes, ports, eapol_tx, trace[64],trace_n;
static int entropy_return=1, reject_key, drop_reply, drop_key_reply, uncertain_tx;
/* The firmware confirms the next EAPOL frame as processed but unacknowledged (tx_done and retry_required, no ack bit). */
static int unacked_once;
static unsigned hold_response,expected_eapol;
static uint8_t delayed[512];
/* The window between message 4 and the open port, as the board sees it: the dispatcher owns the receive, and each
 * firmware reply arrives in an aggregate with the other records the reference receive path handles in a station
 * (aicwf_sdio.h record types 0x00 data, 0x11 messages, 0x12 data confirmations, 0x13 prints; rwnx_msg_rx.c handlers).
 * window: 0 off, 1 the extra records before and after each reply. */
static struct aic8800_dispatch dispatcher;
static int window;
static unsigned window_records,window_data,window_events;
static uint64_t window_pn=20,window_latency,window_release;
static uint8_t window_held[AIC8800_TRANSPORT_RX_BYTES],window_held_blocks;
static unsigned rx_frame(uint8_t *p,const uint8_t *eapol,unsigned n,int encrypted,uint64_t pn,unsigned keyid,int group);
static unsigned window_data_record(uint8_t *p,int encrypted,int group,uint16_t ethertype)
{
    static const uint8_t arp[28]={0,1,8,0,6,4,0,1};
    unsigned n=rx_frame(p,arp,sizeof(arp),encrypted,window_pn++,1,group),iv=encrypted?8:0;
    p[84+iv+6]=(uint8_t)(ethertype>>8);p[84+iv+7]=(uint8_t)ethertype;return n;
}
static void window_surround(void)
{
    if(!window||!m.card[1][4])return;
    uint8_t reply[512];unsigned bytes=((unsigned)receive[0]|(unsigned)receive[1]<<8)+4;bytes=(bytes+3)&~3u;
    assert(bytes<=sizeof(reply));memcpy(reply,receive,bytes);memset(receive,0,sizeof(receive));
    unsigned at=0;
    receive[0]=3;receive[2]=0x13;receive[4]='o';receive[5]='k';receive[6]='\n';at=8;     /* firmware print */
    const uint8_t credits[4]={3,0,8,0};at+=message(receive+at,0x140b,credits,4);          /* ME_TX_CREDITS_UPDATE_IND */
    at+=window_data_record(receive+at,1,1,0x0806);                                       /* group-addressed, CCMP, key 1 */
    memcpy(receive+at,reply,bytes);at+=bytes;
    at+=window_data_record(receive+at,1,1,0x86dd);
    at+=window_data_record(receive+at,0,0,0x0800);                                       /* unprotected unicast */
    m.card[1][4]=(uint8_t)((at+511)/512);
    if(window_latency){
        /* The firmware answers later than the bus: the reply waits, the card's function interrupt low. */
        assert(!window_held_blocks);memcpy(window_held,receive,sizeof(window_held));window_held_blocks=m.card[1][4];
        window_release=m.now+window_latency;memset(receive,0,sizeof(receive));m.card[1][4]=0;
    }
}
/* late-completion: the first clock read after a confirmed key add returns the command's deadline. */
static int late_completion;
static int late_now(void *context,uint64_t *out)
{
    if(late_completion==1&&security.state==AIC8800_SECURITY_DONE&&security.operation==AIC8800_SECURITY_ADD&&
        security.completion_deadline){m.now=security.completion_deadline;late_completion=2;}
    return now_cb(context,out);
}
static int entropy(void *ctx,uint8_t *out,size_t bytes)
{
    assert(ctx==&m&&bytes==32);++entropy_calls;
    for(unsigned i=0;i<bytes;++i)out[i]=(uint8_t)(128+i);
    return entropy_return;
}
static void control_reply(uint16_t id,const uint8_t *p,unsigned n)
{
    memset(receive,0,sizeof(receive));message(receive,id,p,n);m.card[1][4]=1;
}
static int security_write(void *ctx,uint32_t off,uint32_t value)
{
    if(uncertain_tx&&off==0x200&&m.write&&m.position==4){++m.accesses;return 0;}
    int ok=write_cb(ctx,off,value);
    if(off==0x200&&m.write&&m.position==m.bytes) {
        const uint8_t *p=m.transmitted;assert(trace_n<64);
        if(p[2]==1) {
            assert(data.result.eapol);unsigned index=expected_eapol;
            if(index==6)expected_eapol=8;
            assert(sizes[index]+32<=m.bytes);
            if(memcmp(p+32,vectors[index],sizes[index])) {
                fprintf(stderr,"EAPOL mismatch packet %u: length %u expected %u\n",eapol_tx,(unsigned)p[4],sizes[index]);
                for(unsigned i=0;i<sizes[index];++i)if(p[32+i]!=vectors[index][i])fprintf(stderr,"offset %u actual %02x expected %02x\n",i,p[32+i],vectors[index][i]);
                assert(0);
            }
            ++eapol_tx;trace[trace_n++]=0x10000|index;
            memset(receive,0,sizeof(receive));receive[0]=8;receive[2]=0x92;put32(receive+4,unacked_once?3:9);unacked_once=0;put32(receive+8,get32(p+8));m.card[1][4]=1;
        } else {
            assert(p[2]==0x11);unsigned id=p[8]|(unsigned)p[9]<<8;trace[trace_n++]=id;
            if(id==36) {
                assert(p[14]==44&&p[20]==16&&p[56]==2&&p[57]==1);
                unsigned which=p[59]?1:(p[16]==1?2:3);assert(!memcmp(p+24,vectors[which],16));
                ++key_adds;uint8_t cfm[]={reject_key?7:0,(uint8_t)(10+which),0xa5,0x5a};control_reply(37,cfm,4);
                if(drop_key_reply){memset(receive,0,sizeof(receive));m.card[1][4]=0;}
            } else if(id==38) {
                assert(p[16]>=11&&p[16]<=13);++deletes;uint8_t opaque[]={0xa5,0x5a};control_reply(39,opaque,2);
            } else {
                assert(id==5124&&p[14]==2&&p[16]==3&&p[17]<=1);++ports;
                uint8_t opaque[]={0x12,0x34};control_reply(5125,opaque,2);
            }
        }
        window_surround();
        if(hold_response){memcpy(delayed,receive,sizeof(delayed));memset(receive,0,sizeof(receive));m.card[1][4]=0;hold_response=0;}
        if(drop_reply){memset(receive,0,sizeof(receive));m.card[1][4]=0;}
    }
    return ok;
}
static void checkpoint(void)
{
    reset_model();host.state=SUNXI_SDIO_READY;host.card_hz=375000;
    host.config.ops.read32=transport_read;host.config.ops.write32=security_write;
    card.host=&host;card.initialized=1;card.state=SDIO_CARD_READY;card.function_count=2;card.capabilities=2;
    card.functions[1].has_manufacturer=1;card.functions[1].manufacturer=0xc8a1;card.functions[1].device=0x0082;
    card.functions[2].has_manufacturer=1;card.functions[2].manufacturer=0xc8a1;card.functions[2].device=0x0182;
    card.functions[1].max_block_bytes=512;card.functions[1].enable_timeout_ms=100;m.card[1][3]=3;
    memset(&transport,0,sizeof(transport));memset(receive,0,sizeof(receive));memset(&firmware,0,sizeof(firmware));
    memset(&runtime,0,sizeof(runtime));memset(&wifi,0,sizeof(wifi));memset(&data,0,sizeof(data));
    memset(&security,0,sizeof(security));memset(&supplicant,0,sizeof(supplicant));memset(delayed,0,sizeof(delayed));
    struct aic8800_transport_config tc={.variant=AIC8800_TRANSPORT_D80_BOOTSTRAP,.pad=AIC8800_PAD_KEEP,
        .ticks_per_ms=10,.command_timeout=200,.poll_interval=5};
    assert(aic8800_transport_init(&transport,&card,&tc));
    firmware.initialized=1;firmware.owner=&transport;firmware.state=AIC8800_FW_START_CONFIRMED;
    transport.firmware=&firmware;transport.state=AIC8800_TRANSPORT_START_CONFIRMED;
    struct aic8800_runtime_config rc={.read_platform_id=platform_id,.context=&m,.binding_timeout=2000};
    assert(aic8800_runtime_init(&runtime,&rc));assert(aic8800_transport_start_runtime(&transport,&runtime,m.now+100000));
    struct aic8800_wifi_config wc={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=1000,
        .profile=1,.channel_count=1,.channels={{2412,0,1,17}}};
    assert(aic8800_wifi_init(&wifi,&wc));assert(aic8800_transport_start_wifi(&transport,&wifi,m.now+80000));
    wifi.state=AIC8800_WIFI_READY;wifi.vif=1;wifi.max_stations=8;wifi.max_vifs=4;wifi.ap_index=3;
    wifi.link=AIC8800_WIFI_SECURITY_PENDING;wifi.association_generation=1;
    const uint8_t mac[]={2,1,2,3,4,5},ap[]={2,0x11,0x22,0x33,0x44,0x55};
    memcpy(wifi.mac,mac,6);memcpy(wifi.target.bssid,ap,6);memcpy(wifi.target.ssid,"model",5);wifi.target.ssid_bytes=5;
    wifi.target.security=AIC8800_WIFI_WPA2_PSK_CCMP;wifi.target.beacon_evidence=17;
    assert(aic8800_data_init(&data));assert(aic8800_transport_start_data(&transport,&data));
    struct aic8800_supplicant_config sc={.entropy=entropy,.entropy_context=&m,.entropy_record=7,.beacon_record=17,
        .ticks_per_second=1000,.command_timeout=3000,.ap_rsn_bytes=(uint16_t)sizes[4]};
    memcpy(sc.pmk,vectors[0],32);memcpy(sc.ap_rsn,vectors[4],sizes[4]);
    assert(aic8800_security_init(&security));assert(aic8800_supplicant_init(&supplicant,&sc));
#ifdef SUNSTONE_AIC8800_SAE
    /* With the same owner, time and beacon evidence that admit the PSK
     * supplicant below, claim refuses an SAE configuration. */
    static struct aic8800_supplicant sae;struct aic8800_supplicant_config sae_config=sc;
    memset(sae_config.pmk,0,sizeof(sae_config.pmk));sae_config.key_mgmt=AIC8800_SUPPLICANT_SAE;sae_config.ap_rsn[19]=8;
    sae_config.sae_password_bytes=5;memcpy(sae_config.sae_password,"horse",5);
    memset(&sae,0,sizeof(sae));assert(aic8800_supplicant_init(&sae,&sae_config));
    assert(!aic8800_supplicant_claim(&sae,&transport,&security,m.now,m.now+50000)&&!sae.owner&&!security.owner);
#endif
    assert(aic8800_transport_start_security(&transport,&security,&supplicant,m.now+50000));
    entropy_calls=key_adds=deletes=ports=eapol_tx=trace_n=0;entropy_return=1;reject_key=drop_reply=uncertain_tx=unacked_once=0;hold_response=0;expected_eapol=6;
}
static unsigned rx_frame(uint8_t *p,const uint8_t *eapol,unsigned n,int encrypted,uint64_t pn,unsigned keyid,int group)
{
    unsigned iv=encrypted?8:0,total=24+iv+8+n;memset(p,0,60+total);
    p[0]=(uint8_t)total;p[1]=(uint8_t)(total>>8);p[48]=0x40;p[49]=1;p[50]=3;p[51]=0xff;
    if(encrypted)p[36]=12;
    p[60]=8;p[61]=encrypted?0x42:2;memcpy(p+64,wifi.mac,6);if(group)p[64]=1;
    memcpy(p+70,wifi.target.bssid,6);memcpy(p+76,wifi.target.bssid,6);
    if(encrypted){p[84]=(uint8_t)pn;p[85]=(uint8_t)(pn>>8);p[87]=(uint8_t)(0x20|(keyid<<6));for(unsigned i=0;i<4;++i)p[88+i]=(uint8_t)(pn>>(16+8*i));}
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x88,0x8e};memcpy(p+84+iv,snap,8);memcpy(p+92+iv,eapol,n);
    return (60+total+3)&~3u;
}
static void inject(unsigned index)
{
    idle();assert(!m.card[1][4]);memset(receive,0,sizeof(receive));rx_frame(receive,vectors[index],sizes[index],0,0,0,0);m.card[1][4]=1;
}
static void settled(void)
{
    for(unsigned i=0;i<4000;++i) {
        step();assert(!transport.held);
        if(transport.state==AIC8800_TRANSPORT_FAULT)break;
        if(!m.card[1][4]&&!transport.pending&&transport.cursor==transport.rx_bytes&&
            !supplicant.count&&!supplicant.deferred_bytes&&!supplicant.action_active&&!supplicant.port_pending&&
            data.state==AIC8800_DATA_IDLE&&security.state==AIC8800_SECURITY_IDLE&&
            (!supplicant.completed||supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED))return;
    }
    fprintf(stderr,"unsettled transport=%u phase=%u RSN=%u internal=%d data=%u security=%u count=%u now=%llu\n",transport.state,transport.phase,supplicant.state,supplicant.rsn_state,data.state,security.state,supplicant.count,(unsigned long long)m.now);
    assert(0);
}
static void authorized(void)
{
    inject(5);settled();assert(entropy_calls==1&&eapol_tx==1&&!key_adds&&!aic8800_security_open(&security));
    inject(7);settled();assert(key_adds==2&&ports==1&&eapol_tx==2&&aic8800_security_open(&security));
    assert(supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED&&wifi.link==AIC8800_WIFI_SECURED);
    assert(trace_n==5&&trace[0]==0x10006&&trace[1]==0x10008&&trace[2]==36&&trace[3]==36&&trace[4]==5124);
}
static int allzero(const void *p,size_t n){const uint8_t *b=p;for(size_t i=0;i<n;++i)if(b[i])return 0;return 1;}
int main(int argc,char **argv)
{
    assert(argc==3||argc==4);FILE *f=fopen(argv[1],"rb");assert(f);
    for(unsigned i=0;i<15;++i){uint8_t n[2];assert(fread(n,1,2,f)==2);sizes[i]=n[0]|(unsigned)n[1]<<8;assert(sizes[i]<=256&&fread(vectors[i],1,sizes[i],f)==sizes[i]);}
    assert(fgetc(f)==EOF&&!fclose(f));checkpoint();
    if(!strcmp(argv[2],"lifecycle")) {
        authorized();security.keys[0].pn[16]=9;security.keys[2].pn[16]=7;
        inject(7);settled();assert(key_adds==2&&security.keys[0].pn[16]==9&&security.keys[2].pn[16]==7);
        /* Equal replay is discarded by the actual RSN implementation. */
        assert(eapol_tx==2);
        /* Independent group rekey exercises GTK install before its response. */
        eapol_tx=3;expected_eapol=10;inject(9);settled();assert(key_adds==3&&eapol_tx==4&&ports==2&&security.keys[3].pn[16]==5);
        assert(trace[5]==36&&trace[6]==0x1000a&&trace[7]==5124);
        inject(9);settled();assert(key_adds==3&&eapol_tx==4);
        uint8_t record[128]={0},payload[]={1,2,3,4};unsigned n=rx_frame(record,payload,4,1,10,0,0);record[98]=8;record[99]=0;
        idle();memset(receive,0,sizeof(receive));memcpy(receive,record,n);m.card[1][4]=1;until_held();
        struct aic8800_data_frame view;assert(aic8800_transport_peek_data(&transport,&view)&&view.encrypted&&!view.eapol);
        assert(security.keys[0].pn[16]==10);assert(aic8800_transport_consume(&transport));idle();
        /* Replayed decrypted record stays raw for the generic consumer. */
        memset(receive,0,sizeof(receive));memcpy(receive,record,n);m.card[1][4]=1;until_held();assert(!aic8800_transport_peek_data(&transport,&view));
        assert(aic8800_transport_consume(&transport));idle();
        assert(aic8800_transport_retire_security(&transport,m.now+10000));
        for(unsigned i=0;i<3000&&supplicant.state!=AIC8800_SUPPLICANT_RETIRED;++i)step();
        assert(supplicant.state==AIC8800_SUPPLICANT_RETIRED&&deletes==3&&ports==3&&!aic8800_security_open(&security));
        assert(allzero(security.keys,sizeof(security.keys))&&allzero(supplicant.config.pmk,32));
        assert(allzero(supplicant.small,sizeof(supplicant.small))&&allzero(supplicant.large,sizeof(supplicant.large)));
        idle();assert(aic8800_transport_finish_security(&transport));assert(!transport.security&&!transport.supplicant&&!data.security);
        puts("RSN lifecycle: independent four-way/group messages, ordered key/port confirmation, replay PN, decrypted RX and key retirement passed");return 0;
    }
    if(!strcmp(argv[2],"reinstall")) {
        authorized();expected_eapol=10;inject(9);settled();assert(key_adds==3);
        security.keys[0].pn[16]=11;security.keys[2].pn[16]=12;security.keys[3].pn[16]=13;
        expected_eapol=12;inject(11);settled();assert(key_adds==3);
        assert(security.keys[0].pn[16]==11&&security.keys[2].pn[16]==12&&security.keys[3].pn[16]==13);
        expected_eapol=14;inject(13);settled();assert(key_adds==3&&eapol_tx==5);
        assert(security.keys[0].pn[16]==11&&security.keys[2].pn[16]==12&&security.keys[3].pn[16]==13);
        puts("RSN reinstall refusal: authenticated higher-replay M3/group retransmissions resend responses without key or PN reset");return 0;
    }
    if(!strcmp(argv[2],"burst")) {
        inject(5);hold_response=1;
        for(unsigned i=0;i<1000&&!eapol_tx;++i)step();
        assert(eapol_tx==1&&data.state!=AIC8800_DATA_IDLE);
        idle();memset(receive,0,sizeof(receive));unsigned n=rx_frame(receive,vectors[7],sizes[7],0,0,0,0);
        n+=rx_frame(receive+n,vectors[7],sizes[7],0,0,0,0);memcpy(receive+n,delayed,12);m.card[1][4]=(uint8_t)((n+12+511)/512);
        settled();assert(supplicant.dropped_eapol==1&&eapol_tx==2&&key_adds==2&&aic8800_security_open(&security));
        puts("RSN burst: first deferred EAPOL retained, excess retransmission dropped, CFM progresses and keys authorize in order");return 0;
    }
    if(!strcmp(argv[2],"unacked")) {
        /* As in the reference SDIO path, an unacknowledged message 2 is not a fault: message 3 still completes the
         * handshake (the access point received it although the acknowledgement was lost). */
        unacked_once=1;authorized();assert(supplicant.eapol_tx==2);
        puts("RSN unacknowledged EAPOL: a processed but unacknowledged message 2 does not stop the handshake");return 0;
    }
    if(!strcmp(argv[2],"bad-mic")) {
        inject(5);settled();vectors[7][81]^=1;inject(7);settled();
        assert(!key_adds&&!ports&&eapol_tx==1&&!aic8800_security_open(&security));
        vectors[7][81]^=1;vectors[7][2]=0xff;inject(7);until_held();
        struct aic8800_data_frame invalid;assert(!aic8800_transport_peek_data(&transport,&invalid));
        assert(aic8800_transport_consume(&transport));idle();
        assert(!key_adds&&!ports&&eapol_tx==1);m.now=supplicant.deadline;step();
        assert(supplicant.state==AIC8800_SUPPLICANT_FAULT);retained_fault();
        puts("RSN input refusal: invalid MIC and oversized EAPOL length do not install keys or authorize data");return 0;
    }
    if(!strcmp(argv[2],"window")) {
        /* The board's receive ownership: the session's management stage drives the transport only through the
         * dispatcher, discards ordinary data and drains events and diagnostic records, as a733_radio_session.c and
         * apps/board/radio.c do. */
        assert(aic8800_dispatch_init(&dispatcher,&transport));window=1;
        /* window-events: the board's interrupt mode. The owner polls only when the transport reports work or the
         * controller latches the card's function interrupt, and otherwise sleeps to the transport's next deadline. */
        int events=argc>3&&!strcmp(argv[3],"events");unsigned sleeps=0;
        if(events){host.events=1;m.allow_irq_ack=1;window_latency=20;}
        for(unsigned i=0;i<20000;++i) {
            if(window_held_blocks&&!m.card[1][4]&&m.now>=window_release){
                memcpy(receive,window_held,sizeof(receive));m.card[1][4]=window_held_blocks;window_held_blocks=0;}
            if(events) {
                m.irq_level=m.card[1][4]!=0;uint64_t until=0;
                /* A command in flight completes in microseconds on the board; only an idle bus sleeps. */
                if(!transport.pending&&!aic8800_transport_event_work(&transport,m.now)&&sunxi_sdio_event_prepare(&host,&until)==0) {
                    uint64_t next=aic8800_transport_event_deadline(&transport,m.now);
                    if(until&&(!next||until<next))next=until;
                    if(window_held_blocks&&window_release>m.now&&(!next||window_release<next))next=window_release;
                    assert(next>m.now);m.now=next;++sleeps;
                }else ++m.now;
            }else ++m.now;
            aic8800_dispatch_poll(&dispatcher);
            struct aic8800_data_frame f;
            if(aic8800_dispatch_peek_data(&dispatcher,&f)){assert(aic8800_dispatch_consume_data(&dispatcher));++window_data;}
            struct aic8800_dispatch_event e;while(aic8800_dispatch_take_event(&dispatcher,&e))++window_events;
            struct aic8800_dispatch_record r;while(aic8800_dispatch_take_record(&dispatcher,&r))++window_records;
            if(transport.state==AIC8800_TRANSPORT_FAULT||dispatcher.faulted)break;
            int quiet=!m.card[1][4]&&!window_held_blocks&&!transport.pending&&!transport.held&&transport.cursor==transport.rx_bytes&&
                !supplicant.count&&!supplicant.action_active&&!supplicant.port_pending&&!supplicant.deferred_bytes&&
                data.state==AIC8800_DATA_IDLE&&security.state==AIC8800_SECURITY_IDLE;
            if(quiet&&!eapol_tx){memset(receive,0,sizeof(receive));rx_frame(receive,vectors[5],sizes[5],0,0,0,0);m.card[1][4]=1;}
            else if(quiet&&eapol_tx==1&&!key_adds){memset(receive,0,sizeof(receive));rx_frame(receive,vectors[7],sizes[7],0,0,0,0);m.card[1][4]=1;}
            else if(quiet&&supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED)break;
        }
        fprintf(stderr,"window%s: sleeps=%u now=%llu transport=%u error=%u dispatcher=%u RSN=%u internal=%d data=%u/%u security=%u op=%u eapol=%u keys=%u ports=%u records=%u data=%u events=%u\n",events?"-events":"",sleeps,(unsigned long long)m.now,
            transport.state,transport.error,dispatcher.faulted,supplicant.state,supplicant.rsn_state,data.state,data.error,
            security.state,security.operation,eapol_tx,key_adds,ports,window_records,window_data,window_events);
        assert(transport.state!=AIC8800_TRANSPORT_FAULT&&!dispatcher.faulted);
        assert(supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED&&wifi.link==AIC8800_WIFI_SECURED&&key_adds==2&&ports==1);
        puts("RSN window: message 4 to the open port with the station's other receive records through the dispatcher");return 0;
    }
    if(!strcmp(argv[2],"entropy")){entropy_return=-1;inject(5);}
    else if(!strcmp(argv[2],"key-failure")){inject(5);settled();reject_key=1;inject(7);}
    else if(!strcmp(argv[2],"tx-uncertain")){uncertain_tx=1;inject(5);}
    else if(!strcmp(argv[2],"timeout")){drop_reply=1;inject(5);}
    else if(!strcmp(argv[2],"key-timeout")){inject(5);settled();drop_key_reply=1;inject(7);}
    else if(!strcmp(argv[2],"late-completion")) {
        /* The only observation after a key add's confirmation is the clock read that ends the same transport poll
         * (aic8800_transport_poll's post label). Here that read lands on the command's deadline: the pass that
         * parsed the confirmation ran late. */
        inject(5);settled();late_completion=1;host.config.ops.now=late_now;inject(7);
    }
    else if(!strcmp(argv[2],"key-link")) {
        /* The link leaves security-pending while a key add awaits its confirmation. */
        inject(5);settled();drop_key_reply=1;inject(7);
        for(unsigned i=0;i<5000&&security.state!=AIC8800_SECURITY_WAITING;++i)step();
        assert(security.state==AIC8800_SECURITY_WAITING);wifi.link=AIC8800_WIFI_DISCONNECTING;
    }
    else if(!strcmp(argv[2],"generation")){authorized();++wifi.association_generation;}
    else assert(0);
    for(unsigned i=0;i<5000&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(supplicant.state==AIC8800_SUPPLICANT_FAULT&&!aic8800_security_open(&security));
    assert(supplicant.owner==&transport&&security.owner==&transport&&!allzero(supplicant.config.pmk,32));
    /* The record's fault site separates a confirmation that never came from one the firmware refused. */
    struct aic8800_fault_site site;aic8800_fault_site_capture(&site,&transport,&supplicant,1234);
    if(!strcmp(argv[2],"late-completion")) {
        fprintf(stderr,"site: transport %#x/%u data %#x security %#x slot %#x cfm %u/%u/%u supplicant %#x actions %#x stop %#x rsn %d eapol %u\n",
            site.transport_site,site.transport_error,site.data,site.security,site.slot,site.expected_cfm,site.cfm_bytes,
            site.cfm_status,site.supplicant,site.actions,site.stop_site,supplicant.rsn_state,eapol_tx);
        assert((site.slot>>4)==AIC8800_SECURITY_STOP_COMPLETION&&(site.security&15u)==AIC8800_SECURITY_DONE&&
            site.cfm_bytes==4&&!site.cfm_status&&supplicant.rsn_state==9&&eapol_tx==2);
        printf("fault site: a late observation of a confirmed key add names the completion\n");
    }
    if(!strcmp(argv[2],"key-timeout")||!strcmp(argv[2],"key-failure")||!strcmp(argv[2],"key-link")) {
        unsigned refused=!strcmp(argv[2],"key-failure"),link=!strcmp(argv[2],"key-link");
        fprintf(stderr,"site: transport %#x/%u data %#x security %#x slot %u cfm %u/%u/%u supplicant %#x actions %#x stop %#x\n",
            site.transport_site,site.transport_error,site.data,site.security,site.slot,site.expected_cfm,site.cfm_bytes,
            site.cfm_status,site.supplicant,site.actions,site.stop_site);
        assert(site.transport_error==AIC8800_TRANSPORT_DATA_ERROR&&(site.transport_site>>12)==4u&&site.ms==1234);
        assert((site.security&15u)==AIC8800_SECURITY_WAITING&&(site.security&16u)&&
            (site.security>>5)==AIC8800_SECURITY_ADD&&site.expected_cfm==37&&(site.slot&7u)==0&&
            (site.slot>>4)==(refused?AIC8800_SECURITY_STOP_CONFIRMATION:link?AIC8800_SECURITY_STOP_LINK:AIC8800_SECURITY_STOP_DEADLINE));
        assert(refused?site.cfm_bytes==4&&site.cfm_status==7:!site.cfm_bytes&&!site.cfm_status);
        assert((site.supplicant&15u)==AIC8800_SUPPLICANT_FAULT&&(site.actions&1u)&&(site.actions&2u)&&(site.actions&8u));
        /* The security worker stopped first; the transport's fault then stopped the supplicant. */
        assert((site.stop_site>>12)==4u);
        printf("fault site: %s\n",refused?"refused key add names its status":link?"a link change while a key add waits names the link":
            "unanswered key add names the outstanding confirmation");
    }
    retained_fault();
    assert(!aic8800_transport_finish_security(&transport));
    puts("RSN refusal: terminal fault retains ownership and denies ordinary data");return 0;
}
