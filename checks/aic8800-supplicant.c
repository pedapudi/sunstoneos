/* Actual BSD RSN state/crypto + native key/data/transport/card/controller.
 * Firmware execution, association and decrypted RX are explicit model inputs. */
#define AIC8800_RUNTIME_FIXTURE_MAIN security_runtime_unused_main
#include "aic8800-runtime.c"
#undef AIC8800_RUNTIME_FIXTURE_MAIN
#include "aic8800_supplicant.h"
static struct aic8800_wifi wifi;
static struct aic8800_data data;
static struct aic8800_security security;
static struct aic8800_supplicant supplicant;
static uint8_t vectors[15][256];static unsigned sizes[15];
static unsigned entropy_calls, key_adds, deletes, ports, eapol_tx, trace[64],trace_n;
static int entropy_return=1, reject_key, drop_reply, uncertain_tx;
static unsigned hold_response,expected_eapol;
static uint8_t delayed[512];
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
            memset(receive,0,sizeof(receive));receive[0]=8;receive[2]=0x92;put32(receive+4,9);put32(receive+8,get32(p+8));m.card[1][4]=1;
        } else {
            assert(p[2]==0x11);unsigned id=p[8]|(unsigned)p[9]<<8;trace[trace_n++]=id;
            if(id==36) {
                assert(p[14]==44&&p[20]==16&&p[56]==2&&p[57]==1);
                unsigned which=p[59]?1:(p[16]==1?2:3);assert(!memcmp(p+24,vectors[which],16));
                ++key_adds;uint8_t cfm[]={reject_key?7:0,(uint8_t)(10+which),0xa5,0x5a};control_reply(37,cfm,4);
            } else if(id==38) {
                assert(p[16]>=11&&p[16]<=13);++deletes;uint8_t opaque[]={0xa5,0x5a};control_reply(39,opaque,2);
            } else {
                assert(id==5124&&p[14]==2&&p[16]==3&&p[17]<=1);++ports;
                uint8_t opaque[]={0x12,0x34};control_reply(5125,opaque,2);
            }
        }
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
    card.functions[1].has_manufacturer=1;card.functions[1].manufacturer=0xc8a1;card.functions[1].device=0x0182;
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
    assert(aic8800_transport_start_security(&transport,&security,&supplicant,m.now+50000));
    entropy_calls=key_adds=deletes=ports=eapol_tx=trace_n=0;entropy_return=1;reject_key=drop_reply=uncertain_tx=0;hold_response=0;expected_eapol=6;
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
    assert(argc==3);FILE *f=fopen(argv[1],"rb");assert(f);
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
    if(!strcmp(argv[2],"entropy")){entropy_return=-1;inject(5);}
    else if(!strcmp(argv[2],"key-failure")){inject(5);settled();reject_key=1;inject(7);}
    else if(!strcmp(argv[2],"tx-uncertain")){uncertain_tx=1;inject(5);}
    else if(!strcmp(argv[2],"timeout")){drop_reply=1;inject(5);}
    else if(!strcmp(argv[2],"generation")){authorized();++wifi.association_generation;}
    else assert(0);
    for(unsigned i=0;i<5000&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(supplicant.state==AIC8800_SUPPLICANT_FAULT&&!aic8800_security_open(&security));
    assert(supplicant.owner==&transport&&security.owner==&transport&&!allzero(supplicant.config.pmk,32));retained_fault();
    assert(!aic8800_transport_finish_security(&transport));
    puts("RSN refusal: terminal fault retains ownership and denies ordinary data");return 0;
}
