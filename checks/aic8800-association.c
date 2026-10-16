/* Reuse register callbacks only; establish an explicit initialized checkpoint. */
#define AIC8800_RUNTIME_FIXTURE_MAIN association_runtime_unused_main
#include "aic8800-runtime.c"
#undef AIC8800_RUNTIME_FIXTURE_MAIN
#include "aic8800_wifi.h"
static struct aic8800_wifi wifi;
static uint8_t packets[3][512], indication[852];
static unsigned sent, station_events, foreign_events;
static int indication_first, bad_lengths, wrong_target, refused, failed_status, drop_indication, partial_tx;
static int late_final, spontaneous;
static unsigned wire(uint8_t *p,uint16_t id,const uint8_t *params,unsigned n)
{
    unsigned length=12+n;p[0]=(uint8_t)length;p[1]=(uint8_t)(length>>8);p[2]=0x11;
    p[4]=(uint8_t)id;p[5]=(uint8_t)(id>>8);p[10]=(uint8_t)n;p[11]=(uint8_t)(n>>8);
    if(n)memcpy(p+16,params,n);
    return (16+n+3)&~3u;
}
static int station_clock(void *ctx,uint64_t *out)
{
    if(late_final&&wifi.state==AIC8800_WIFI_READY&&wifi.station_indicated)m.now=wifi.deadline;
    return now_cb(ctx,out);
}
static int station_write(void *ctx,uint32_t off,uint32_t value)
{
    if(partial_tx&&off==0x200&&m.write&&m.position==4){++m.accesses;return 0;}
    int result=write_cb(ctx,off,value);
    if(off!=0x200||!m.write||m.position!=m.bytes)return result;
    unsigned which=m.transmitted[8]==3?2:wifi.target.security==AIC8800_WIFI_OPEN?0:1;
    assert(m.bytes==512&&!memcmp(m.transmitted,packets[which],512));++sent;
    memset(receive,0,sizeof(receive));unsigned at=0;uint8_t status=(uint8_t)refused;
    if(which==2) {
        uint8_t loss[6]={3,0,1,0,0,0xa5};
        if(indication_first)at=wire(receive,0x1805,loss,6);
        uint8_t opaque[]={0xa5,0x5a};at+=wire(receive+at,0x1804,opaque,2);
        if(!indication_first&&!drop_indication)at+=wire(receive+at,0x1805,loss,6);
    }else {
        uint8_t ind[852];memcpy(ind,indication,sizeof(ind));
        if(bad_lengths){ind[14]=0xff;ind[15]=0xff;}
        if(wrong_target)ind[2]^=4;
        if(failed_status){ind[0]=(uint8_t)failed_status;memset(ind+2,0,6);}
        /* Unrelated VIF must remain a raw owned record. */
        ind[9]=2;at=wire(receive,0x1802,ind,sizeof(ind));ind[9]=1;
        if(indication_first&&!refused)at+=wire(receive+at,0x1802,ind,sizeof(ind));
        at+=wire(receive+at,0x1801,&status,1);
        if(!indication_first&&!refused&&!drop_indication)at+=wire(receive+at,0x1802,ind,sizeof(ind));
    }
    assert(at<=2048);m.card[1][4]=(uint8_t)((at+511)/512);return result;
}
static void checkpoint(void)
{
    reset_model();host.state=SUNXI_SDIO_READY;host.card_hz=375000;
    host.config.ops.read32=transport_read;host.config.ops.write32=station_write;host.config.ops.now=station_clock;
    /* No enumeration/bootstrap/upload/management execution is claimed. */
    card.host=&host;card.initialized=1;card.state=SDIO_CARD_READY;card.function_count=2;card.capabilities=2;
    card.functions[1].has_manufacturer=1;card.functions[1].manufacturer=0xc8a1;card.functions[1].device=0x0182;
    card.functions[1].max_block_bytes=512;card.functions[1].enable_timeout_ms=100;
    m.card[1][3]=1;memset(&transport,0,sizeof(transport));memset(receive,0,sizeof(receive));
    memset(&firmware,0,sizeof(firmware));memset(&runtime,0,sizeof(runtime));memset(&wifi,0,sizeof(wifi));
    struct aic8800_transport_config tc={.variant=AIC8800_TRANSPORT_D80_BOOTSTRAP,.pad=AIC8800_PAD_KEEP,
        .ticks_per_ms=10,.command_timeout=200,.poll_interval=5};
    assert(aic8800_transport_init(&transport,&card,&tc));
    firmware.initialized=1;firmware.owner=&transport;firmware.state=AIC8800_FW_START_CONFIRMED;
    transport.firmware=&firmware;transport.state=AIC8800_TRANSPORT_START_CONFIRMED;
    struct aic8800_runtime_config rc={.read_platform_id=platform_id,.context=&m,.binding_timeout=2000};
    assert(aic8800_runtime_init(&runtime,&rc));assert(aic8800_transport_start_runtime(&transport,&runtime,m.now+10000));
    struct aic8800_wifi_config wc={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=1000,
        .profile=1,.channel_count=1,.channels={{2412,0,1,17}}};
    assert(aic8800_wifi_init(&wifi,&wc));assert(aic8800_transport_start_wifi(&transport,&wifi,m.now+8000));
    wifi.state=AIC8800_WIFI_READY;wifi.vif=1;wifi.max_stations=8;wifi.max_vifs=4;
    sent=station_events=foreign_events=0;
    indication_first=bad_lengths=wrong_target=refused=failed_status=drop_indication=partial_tx=late_final=spontaneous=0;
}
static struct aic8800_wifi_target target(unsigned secure)
{
    struct aic8800_wifi_target t={.connection_authority=3,.beacon_evidence=4,
        .security=secure?AIC8800_WIFI_WPA2_PSK_CCMP:AIC8800_WIFI_OPEN,
        .bssid={2,0x11,0x22,0x33,0x44,0x55},.ssid={'t','e','s','t'},.ssid_bytes=4};return t;
}
static void drain(void)
{
    for(unsigned i=0;i<1500&&transport.state!=AIC8800_TRANSPORT_FAULT;++i){
        step();
        if(transport.held){
            struct aic8800_wifi_station_event e;
            if(aic8800_transport_peek_station(&transport,&e)) {
                ++station_events;assert(e.payload==wifi.station_payload&&e.vif==1);
                unsigned commands=m.command_count;step();assert(m.command_count==commands);
                if(transport.state==AIC8800_TRANSPORT_FAULT)break;
                assert(!aic8800_transport_peek_station(&transport,(void *)wifi.station_payload));
                assert(!aic8800_transport_disconnect(&transport,3,m.now+1000));
                if(e.kind==AIC8800_WIFI_ASSOCIATION&&!failed_status) {
                    assert(e.ap_index==3&&e.aid==7&&e.frequency==2412&&e.request_ie_bytes==6&&e.response_ie_bytes==3);
                    assert(e.ac_parameters[3]==0x44332211&&e.bytes==852&&e.payload[18]==0xaa);
                    assert(e.link==(wifi.target.security==AIC8800_WIFI_OPEN?AIC8800_WIFI_ASSOCIATED_OPEN:AIC8800_WIFI_SECURITY_PENDING));
                }
            }else {++foreign_events;assert(transport.unrelated.message_id==0x1802&&transport.unrelated.parameters[9]==2);}
            assert(aic8800_transport_consume(&transport));
        }
        if(wifi.state==AIC8800_WIFI_READY&&sent&&transport.cursor==transport.rx_bytes&&!transport.pending)break;
    }
}
static void faulted(void)
{
    assert(wifi.state==AIC8800_WIFI_FAULT);retained_fault();
    assert(wifi.target.connection_authority==3&&wifi.target.beacon_evidence==4);
}
int main(int argc,char **argv)
{
    assert(argc==3||(argc==4&&(!strcmp(argv[3],"--transition")||!strcmp(argv[3],"--busy")||!strcmp(argv[3],"--unresolved"))));FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(packets,512,3,f)==3&&fgetc(f)==EOF);assert(!fclose(f));
    f=fopen(argv[2],"rb");assert(f);assert(fread(indication,1,852,f)==852&&fgetc(f)==EOF);assert(!fclose(f));
    if(argc==4&&!strcmp(argv[3],"--unresolved")) {
        for(unsigned successful=0;successful<2;++successful) {
            checkpoint();struct aic8800_wifi_target t=target(0);
            assert(aic8800_transport_connect(&transport,&t,m.now+4000));
            aic8800_wifi_step(&wifi,&transport,m.now);assert(aic8800_wifi_sent(&wifi,&transport));
            uint8_t ind[852];memcpy(ind,indication,sizeof(ind));ind[8]=1;
            struct aic8800_sdio_response r={.record=ind,.has_message=1,.message_id=0x1802,
                .parameters=ind,.parameter_bytes=sizeof(ind)};
            assert(aic8800_wifi_response(&wifi,&transport,&r,m.now)==AIC8800_CODEC_UNRELATED);
            assert(wifi.link==AIC8800_WIFI_LINK_UNRESOLVED&&!wifi.station_indicated);
            uint8_t status=successful?0:7;r.record=&status;r.parameters=&status;r.parameter_bytes=1;r.message_id=0x1801;
            assert(aic8800_wifi_response(&wifi,&transport,&r,m.now)==
                (successful?AIC8800_CODEC_OK:AIC8800_CODEC_UNRELATED));
            if(successful) {
                ind[8]=0;r.record=ind;r.parameters=ind;r.parameter_bytes=sizeof(ind);r.message_id=0x1802;
                assert(aic8800_wifi_response(&wifi,&transport,&r,m.now)==AIC8800_CODEC_UNRELATED);
            }
            assert(wifi.link==AIC8800_WIFI_LINK_UNRESOLVED&&wifi.state==AIC8800_WIFI_READY);
            assert(wifi.station_event.link==AIC8800_WIFI_LINK_UNRESOLVED);
            aic8800_wifi_consumed(&wifi,&transport,r.record);
            assert(!aic8800_transport_connect(&transport,&t,m.now+1000));
            assert(!aic8800_transport_scan(&transport,m.now+1000));
        }
        puts("AIC station: unresolved roaming survives later refusal or normal association completion");return 0;
    }
    if(argc==4&&!strcmp(argv[3],"--busy")) {
        const uint8_t statuses[]={8,9,255};
        for(unsigned i=0;i<sizeof(statuses);++i) {
            checkpoint();struct aic8800_wifi_target t=target(0);
            assert(aic8800_transport_connect(&transport,&t,m.now+4000));
            aic8800_wifi_step(&wifi,&transport,m.now);assert(aic8800_wifi_sent(&wifi,&transport));
            struct aic8800_sdio_response r={.record=&statuses[i],.has_message=1,.message_id=0x1801,
                .parameters=&statuses[i],.parameter_bytes=1};
            assert(aic8800_wifi_response(&wifi,&transport,&r,m.now)==AIC8800_CODEC_UNRELATED);
            assert(wifi.state==AIC8800_WIFI_READY&&wifi.link==AIC8800_WIFI_LINK_UNRESOLVED);
            assert(wifi.station_event.status==statuses[i]);aic8800_wifi_consumed(&wifi,&transport,r.record);
            assert(!aic8800_transport_connect(&transport,&t,m.now+1000));
            assert(!aic8800_transport_scan(&transport,m.now+1000));
        }
        puts("AIC station: firmware busy/in-progress/unknown refusal retains unresolved ownership");return 0;
    }
    if(argc==4) {
        for(unsigned flag=0;flag<3;++flag) {
            checkpoint();wifi.link=AIC8800_WIFI_SECURITY_PENDING;
            uint8_t loss[6]={4,0,1,(uint8_t)(flag==0),(uint8_t)(flag==1),0xbb};
            if(flag<2)wire(receive,0x1805,loss,6);
            else {uint8_t ind[852];memcpy(ind,indication,sizeof(ind));ind[8]=1;wire(receive,0x1802,ind,sizeof(ind));}
            m.card[1][4]=flag<2?1:2;
            for(unsigned i=0;i<500&&!transport.held;++i)step();
            assert(transport.held&&wifi.link==AIC8800_WIFI_LINK_UNRESOLVED);
            if(flag<2) {struct aic8800_wifi_station_event e;assert(aic8800_transport_peek_station(&transport,&e));
                assert(e.link==AIC8800_WIFI_LINK_UNRESOLVED&&e.status==4);}
            else assert(transport.unrelated.message_id==0x1802);
            assert(aic8800_transport_consume(&transport));idle();
            struct aic8800_wifi_target t=target(0);
            assert(!aic8800_transport_connect(&transport,&t,m.now+1000));
            assert(!aic8800_transport_disconnect(&transport,3,m.now+1000));
            assert(!aic8800_transport_scan(&transport,m.now+1000));
        }
        puts("AIC station: FT/reassociation/roaming retain unresolved link and exclude new operations");return 0;
    }
    checkpoint();struct aic8800_wifi_target t=target(0);t.beacon_evidence=0;
    assert(!aic8800_transport_connect(&transport,&t,m.now+4000));t.beacon_evidence=4;
    assert(aic8800_transport_connect(&transport,&t,m.now+4000));memset(&t,0,sizeof(t));drain();
    assert(wifi.state==AIC8800_WIFI_READY&&wifi.link==AIC8800_WIFI_ASSOCIATED_OPEN&&station_events==1&&foreign_events==1);
    assert(!aic8800_transport_scan(&transport,m.now+1000));indication_first=1;
    assert(aic8800_transport_disconnect(&transport,3,m.now+4000));drain();
    assert(wifi.state==AIC8800_WIFI_READY&&wifi.link==AIC8800_WIFI_LINK_DOWN&&station_events==2);
    t=target(1);assert(aic8800_transport_connect(&transport,&t,m.now+4000));drain();
    assert(wifi.link==AIC8800_WIFI_SECURITY_PENDING&&wifi.state==AIC8800_WIFI_READY&&station_events==3);
    /* Spontaneous disconnect without a request retains flags; no automatic roam. */
    memset(receive,0,sizeof(receive));uint8_t loss[]={4,0,1,1,1,0xbb};wire(receive,0x1805,loss,6);m.card[1][4]=1;
    for(unsigned i=0;i<500&&!transport.held;++i)step();
    struct aic8800_wifi_station_event e;assert(aic8800_transport_peek_station(&transport,&e));
    assert(e.kind==AIC8800_WIFI_DISCONNECTION&&e.status==4&&e.ft_over_ds==1&&e.reassoc==1&&wifi.link==AIC8800_WIFI_LINK_UNRESOLVED);
    assert(aic8800_transport_consume(&transport));

    checkpoint();refused=7;t=target(0);assert(aic8800_transport_connect(&transport,&t,m.now+4000));drain();
    assert(wifi.state==AIC8800_WIFI_READY&&wifi.link==AIC8800_WIFI_LINK_DOWN&&wifi.station_event.status==7);
    checkpoint();failed_status=17;assert(aic8800_transport_connect(&transport,&t,m.now+4000));drain();
    assert(wifi.state==AIC8800_WIFI_READY&&wifi.link==AIC8800_WIFI_LINK_DOWN&&wifi.station_event.status==17);
    checkpoint();bad_lengths=1;assert(aic8800_transport_connect(&transport,&t,m.now+4000));drain();faulted();
    checkpoint();wrong_target=1;assert(aic8800_transport_connect(&transport,&t,m.now+4000));drain();faulted();
    checkpoint();partial_tx=1;assert(aic8800_transport_connect(&transport,&t,m.now+4000));drain();
    assert(m.position==4&&host.request.data==transport.tx);faulted();
    checkpoint();drop_indication=1;assert(aic8800_transport_connect(&transport,&t,m.now+2000));drain();
    m.now=wifi.deadline;step();faulted();
    checkpoint();late_final=1;assert(aic8800_transport_connect(&transport,&t,m.now+4000));drain();
    assert(wifi.station_indicated&&wifi.station_payload[9]==1);faulted();
    puts("AIC station: source ABI packets, open/WPA2 association pending security, disconnect ordering, bounds, ownership and retained failures passed");return 0;
}
