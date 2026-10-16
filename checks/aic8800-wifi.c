#define AIC8800_RUNTIME_FIXTURE_MAIN wifi_runtime_fixture_unused_main
#include "aic8800-runtime.c"
#undef AIC8800_RUNTIME_FIXTURE_MAIN
#include "aic8800_wifi.h"
static struct aic8800_wifi wifi, ready_wifi;
static struct aic8800_sdio_transport ready_transport;
static struct aic8800_runtime ready_runtime;
static struct aic8800_firmware ready_firmware;
static struct sunxi_sdio ready_host;
static struct sdio_card ready_card;
static struct model ready_model;
static uint8_t transcript[11][512];
static unsigned command_at, scan_count;
static int drop_scan, corrupt_result, fail_scan_tx, final_status;
static int late_completion_clock(void *ctx,uint64_t *now)
{
    (void)ctx;
    if(wifi.state==AIC8800_WIFI_SCAN_DONE)m.now=wifi.deadline;
    *now=m.now;return 1;
}

static unsigned result_record(uint8_t *p,int foreign)
{
    uint8_t params[54]={0};params[0]=42;params[2]=0x80;params[4]=0x6c;params[5]=9;
    params[7]=255;params[8]=foreign?2:1;params[9]=(uint8_t)-48;
    params[12]=0x80;
    for(unsigned i=0;i<6;++i)params[28+i]=(uint8_t)(0x22+i);
    params[48]=0;params[49]=corrupt_result?31:4;memcpy(params+50,"test",4);
    return message(p,0x1004,params,sizeof(params));
}
static int wifi_write(void *ctx,uint32_t off,uint32_t value)
{
    if(fail_scan_tx&&off==0x200&&m.write&&m.position==4){++m.accesses;return 0;}
    int result=write_cb(ctx,off,value);
    if(off!=0x200||!m.write||m.position!=m.bytes)return result;
    assert(command_at<11&&m.bytes==512&&!memcmp(m.transmitted,transcript[command_at],512));++command_at;
    uint16_t id=(uint16_t)(m.transmitted[8]|(uint16_t)m.transmitted[9]<<8);
    memset(receive,0,sizeof(receive));uint8_t p[28]={0};unsigned n=0,at=0;uint16_t cfm=id+1;
    if(id==0x69){n=16;for(unsigned i=0;i<4;++i)put32(p+4*i,0x200000+i*0x1000);}
    else if(id==0x73){n=6;p[0]=2;for(unsigned i=1;i<6;++i)p[i]=(uint8_t)i;}
    else if(id==4){
        n=28;put32(p,0x06090101);put32(p+20,1u<<8);p[24]=8;p[26]=4;
        /* Binding arrives while VERSION still awaits CFM. The same aggregate
         * supplies that CFM before binding may become the next transmitted op. */
        at=message(receive,0x417,challenge_bytes,16);
    }else if(id==6){n=2;p[1]=1;}
    else if(id==0x415){n=1;assert(platform_calls==1);}
    else if(id==0x1000){
        ++scan_count;if(drop_scan)return result;
        p[0]=0xa5;p[1]=0x5a;at=message(receive,0x1009,p,2); /* Source ignores this CFM payload. */
        at+=result_record(receive+at,1);at+=result_record(receive+at,0);
        p[0]=1;p[1]=(uint8_t)final_status;p[2]=1;message(receive+at,0x1001,p,3);
        m.card[1][4]=1;return result;
    }else {n=3;p[0]=0x5a;} /* Ignored confirmations have no invented zero status. */
    message(receive+at,cfm,p,n);m.card[1][4]=1;return result;
}
static void save_ready(void)
{
    ready_model=m;ready_host=host;ready_card=card;ready_transport=transport;
    ready_runtime=runtime;ready_firmware=firmware;ready_wifi=wifi;
}
static void restore_ready(void)
{
    m=ready_model;host=ready_host;card=ready_card;transport=ready_transport;
    runtime=ready_runtime;firmware=ready_firmware;wifi=ready_wifi;
    memset(receive,0,sizeof(receive));command_at=10;scan_count=0;
    drop_scan=corrupt_result=fail_scan_tx=final_status=0;
}
static void wifi_fault(void)
{
    assert(wifi.state==AIC8800_WIFI_FAULT);retained_fault();
}
static void run_scan(void)
{
    unsigned seen=0;
    for(unsigned i=0;i<1200&&wifi.state!=AIC8800_WIFI_SCAN_DONE&&wifi.state!=AIC8800_WIFI_FAULT;++i){
        step();
        if(transport.held){
            struct aic8800_wifi_scan_result view;
            if(aic8800_transport_peek_scan(&transport,&view)){
                assert(view.frame_bytes==42&&view.frequency==2412&&view.rssi_dbm==-48);
                assert(view.has_ssid&&view.ssid_bytes==4&&!memcmp(view.ssid,"test",4));
                assert(view.frame==wifi.frame&&!memcmp(view.bssid,"\x22\x23\x24\x25\x26\x27",6));
                unsigned before=m.command_count;step();assert(m.command_count==before);
                assert(!aic8800_transport_peek_scan(&transport,(void *)wifi.frame));
                assert(!aic8800_transport_scan(&transport,m.now+1000));++seen;
            }else{
                struct aic8800_sdio_response raw;assert(aic8800_transport_peek(&transport,&raw));
                assert(raw.message_id==0x1004&&raw.parameters[8]==2); /* Foreign VIF retained. */
            }
            assert(aic8800_transport_consume(&transport));
        }
    }
    if(!corrupt_result)assert(seen==1);
}
int main(int argc,char **argv)
{
    assert(argc==3||(argc==4&&!strcmp(argv[3],"completion-deadline")));FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(transcript,512,11,f)==11&&fgetc(f)==EOF);assert(!fclose(f));
    f=fopen(argv[2],"rb");assert(f);assert(fread(challenge_bytes,1,16,f)==16);assert(!fclose(f));
    if(argc==4) {
        /* The transport observes after parsing; isolate that closing boundary
         * without repeating initialization or scan traffic already checked. */
        struct aic8800_wifi_config c={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=100,
            .profile=1,.channel_count=1,.channels={{2412,0,1,17}}};
        assert(aic8800_wifi_init(&wifi,&c));assert(aic8800_wifi_claim(&wifi,&transport,1,100));
        wifi.state=AIC8800_WIFI_WAITING;wifi.phase=9;wifi.vif=1;wifi.summary.accepted=1;
        uint8_t bytes[3]={1,0,0};struct aic8800_sdio_response r={.has_message=1,.message_id=0x1001,
            .parameter_bytes=3,.parameters=bytes};
        assert(aic8800_wifi_response(&wifi,&transport,&r,99)==AIC8800_CODEC_OK);
        assert(wifi.state==AIC8800_WIFI_SCAN_DONE&&wifi.completion_deadline==100);
        uint64_t now;assert(late_completion_clock(&m,&now));aic8800_wifi_observe(&wifi,&transport,now);
        assert(wifi.state==AIC8800_WIFI_FAULT&&wifi.error==AIC8800_WIFI_TIME_ERROR&&wifi.scan_done);
        /* Also preserve a shorter command bound when advancing to preparation. */
        memset(&wifi,0,sizeof(wifi));assert(aic8800_wifi_init(&wifi,&c));assert(aic8800_wifi_claim(&wifi,&transport,1,100));
        wifi.state=AIC8800_WIFI_WAITING;wifi.phase=3;wifi.expected_cfm=1;wifi.command_deadline=50;
        r.message_id=1;r.parameter_bytes=0;r.parameters=NULL;
        assert(aic8800_wifi_response(&wifi,&transport,&r,49)==AIC8800_CODEC_OK);
        assert(wifi.state==AIC8800_WIFI_PREPARING&&wifi.completion_deadline==50);
        aic8800_wifi_observe(&wifi,&transport,50);
        assert(wifi.state==AIC8800_WIFI_FAULT&&wifi.error==AIC8800_WIFI_TIME_ERROR);
        puts("AIC Wi-Fi: final and intermediate confirmation deadlines survive state changes");return 0;
    }
    begin(AIC8800_PAD_KEEP);setup();identity(2000);
    while(transport.state==AIC8800_TRANSPORT_IDENTITY)step();
    base_transport=transport;base_host=host;base_card=card;base_model=m;
    fresh(0);host.config.ops.write32=wifi_write;
    struct aic8800_wifi_config c={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=500,
        .profile=1,.channel_count=2,.channels={{2412,0,1,17},{5180,1,5,10}}};
    c.channels[0].flags=0;assert(!aic8800_wifi_init(&wifi,&c));c.channels[0].flags=1;
    assert(aic8800_wifi_init(&wifi,&c));assert(!aic8800_wifi_init(&wifi,&c));
    assert(aic8800_transport_start_wifi(&transport,&wifi,m.now+8000));
    for(unsigned i=0;i<3000&&wifi.state!=AIC8800_WIFI_READY;++i)step();
    assert(wifi.state==AIC8800_WIFI_READY&&command_at==10&&runtime.binding_confirmed);
    assert(wifi.calibration_addresses[3]==0x203000&&wifi.mac[0]==2&&wifi.mac[5]==5);
    assert(wifi.version[0]==0x06090101&&wifi.max_stations==8&&wifi.max_vifs==4&&wifi.vif==1);
    idle();save_ready();

    assert(aic8800_transport_scan(&transport,m.now+2000));run_scan();
    assert(wifi.state==AIC8800_WIFI_SCAN_DONE&&wifi.summary.accepted&&wifi.scan_done&&scan_count==1);
    idle();struct aic8800_wifi_scan_summary summary;
    assert(!aic8800_transport_scan(&transport,m.now+1000));
    assert(aic8800_transport_take_scan(&transport,&summary));
    assert(summary.received==1&&summary.reported_results==1&&!summary.status&&wifi.state==AIC8800_WIFI_READY);

    restore_ready();corrupt_result=1;assert(aic8800_transport_scan(&transport,m.now+2000));run_scan();
    assert(wifi.error==AIC8800_WIFI_RESULT_ERROR&&!wifi.held&&wifi.summary.received==0);wifi_fault();
    restore_ready();final_status=7;assert(aic8800_transport_scan(&transport,m.now+2000));run_scan();
    assert(wifi.summary.status==7&&wifi.error==AIC8800_WIFI_STATUS_ERROR);wifi_fault();
    restore_ready();drop_scan=1;assert(aic8800_transport_scan(&transport,m.now+2000));
    for(unsigned i=0;i<500&&wifi.state!=AIC8800_WIFI_WAITING;++i)step();
    assert(scan_count==1);m.now=wifi.command_deadline;step();assert(wifi.error==AIC8800_WIFI_TIME_ERROR);wifi_fault();
    restore_ready();fail_scan_tx=1;assert(aic8800_transport_scan(&transport,m.now+2000));
    for(unsigned i=0;i<500&&wifi.state!=AIC8800_WIFI_FAULT;++i)step();
    assert(m.position==4&&host.request.data==transport.tx&&wifi.request[9]==0x10);wifi_fault();
    puts("AIC Wi-Fi: source ABI/sequence, calibration, management, binding serialization, passive scan and retained failures passed");
    return 0;
}
