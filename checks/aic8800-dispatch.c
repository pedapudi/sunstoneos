/* Actual native network/controller composition. Firmware execution, RF,
 * authorized association, decrypted CCMP and entropy are explicit model inputs.
 * No previous firmware upload or RSN handshake workload is replayed. */
#define kernel_main dispatch_unused_kernel_main
#include "radio-network.c"
#undef kernel_main
static struct aic8800_dispatch bootstrap_dispatch;
static int mode; /* 0 traffic, 1 events, 2 deadline, 3 active CFM, 4 ambiguous. */
static uint64_t decorated_pn;
static unsigned le16(const uint8_t *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static unsigned eapol_records,scan_commands;
static unsigned control(uint8_t *p,uint16_t id,const uint8_t *bytes,unsigned n)
{
    unsigned total=(16+n+3)&~3u;memset(p,0,total);
    p[0]=(uint8_t)(12+n);p[1]=(uint8_t)((12+n)>>8);p[2]=0x91;
    p[4]=(uint8_t)id;p[5]=(uint8_t)(id>>8);p[10]=(uint8_t)n;p[11]=(uint8_t)(n>>8);
    if(n)memcpy(p+16,bytes,n);return total;
}
static unsigned eapol_record(uint8_t *p)
{
    memset(p,0,96);p[0]=36;p[48]=0x40;p[49]=1;p[50]=3;p[51]=0xff;
    p[60]=8;p[61]=2;memcpy(p+64,local_mac,6);memcpy(p+70,peer_mac,6);memcpy(p+76,peer_mac,6);
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x88,0x8e};memcpy(p+84,snap,8);
    p[92]=2;p[93]=1; /* EAPOL-Start: valid EAPOL, no key exchange requested. */
    ++eapol_records;return 96;
}
static int dispatch_clock(void *ctx,uint64_t *now)
{
    int ok=network_clock(ctx,now);
    if(!mode&&peer_pn!=decorated_pn){
        decorated_pn=peer_pn;unsigned bytes=(60+le16(receive)+3)&~3u;
        uint8_t prefix[256]={0};unsigned at=0;
        for(unsigned i=0;i<8;++i){prefix[at]=4;prefix[at+2]=0x93;prefix[at+4]=(uint8_t)i;at+=8;}
        at+=control(prefix+at,0x7ffe,NULL,0);prefix[at-6]=0xff;prefix[at-5]=0xff;
        at+=control(prefix+at,0x0416,NULL,0); /* Stale binding CFM has no current owner. */
        at+=eapol_record(prefix+at);
        assert(at+bytes<=sizeof(receive));memmove(receive+at,receive,bytes);memcpy(receive,prefix,at);
        m.card[1][4]=(uint8_t)((at+bytes+511)/512);
    }
    return ok;
}
static unsigned scan_result_packet(uint8_t *p,unsigned number,unsigned vif)
{
    uint8_t body[53]={0};body[0]=41;body[4]=0x6c;body[5]=9;body[8]=(uint8_t)vif;body[9]=(uint8_t)-40;
    body[12]=0x80;memcpy(body+28,peer_mac,6);body[33]=(uint8_t)number;
    body[48]=0;body[49]=3;body[50]='a';body[51]='p';body[52]=(uint8_t)('0'+number);
    return control(p,0x1004,body,sizeof(body));
}
static int dispatch_write(void *ctx,uint32_t off,uint32_t value)
{
    if(!mode)return network_write(ctx,off,value);
    int ok=write_cb(ctx,off,value);
    if(off==0x200&&m.write&&m.position==m.bytes){
        assert(m.transmitted[2]==0x11&&m.transmitted[8]==0&&m.transmitted[9]==0x10);++scan_commands;
        memset(receive,0,sizeof(receive));unsigned at=0;
        if(mode==3){control(receive,0x1009,NULL,0);receive[10]=0xff;receive[11]=0xff;}
        else if(mode==4){receive[0]=4;receive[2]=0x11;}
        else{
            at+=scan_result_packet(receive+at,0,2);
            const uint8_t foreign_done[]={2,0,5};at+=control(receive+at,0x1001,foreign_done,3);
            uint8_t stale[852]={0};stale[9]=1;at+=control(receive+at,0x1802,stale,sizeof(stale));
            const uint8_t loss[]={3,0,1,0,0,0};at+=control(receive+at,0x1805,loss,6);
            for(unsigned i=0;i<5;++i)at+=scan_result_packet(receive+at,i,1);
            const uint8_t done[]={1,0,5};at+=control(receive+at,0x1001,done,3);
            at+=control(receive+at,0x1001,done,3); /* Duplicate before pending CFM. */
            at+=control(receive+at,0x1009,NULL,0);
        }
        m.card[1][4]=(uint8_t)(((at?at:16)+511)/512);
        if(!m.card[1][4])m.card[1][4]=1;
    }
    return ok;
}
static int rsn_entropy(void *ctx,uint8_t *p,size_t n)
{assert(ctx==&m);for(size_t i=0;i<n;++i)p[i]=(uint8_t)(i+128);return 1;}
static void dispatch_checkpoint(void)
{
    checkpoint();host.config.ops.now=dispatch_clock;host.config.ops.write32=dispatch_write;
    /* The inherited checkpoint has no allocated RSN context. Replace that
     * synthetic object with actual BSD RSN allocation/configuration, then retain
     * the explicitly reconstructed key/port checkpoint for network routing. */
    transport.security=NULL;transport.supplicant=NULL;data.security=NULL;
    memset(&security,0,sizeof(security));memset(&supplicant,0,sizeof(supplicant));
    if(mode){wifi.link=AIC8800_WIFI_LINK_DOWN;return;}
    wifi.link=AIC8800_WIFI_SECURITY_PENDING;wifi.target.beacon_evidence=17;
    struct aic8800_supplicant_config c={.entropy=rsn_entropy,.entropy_context=&m,.entropy_record=1,
        .beacon_record=17,.ticks_per_second=1000,.command_timeout=3000,.ap_rsn_bytes=22};
    const uint8_t rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    memcpy(c.ap_rsn,rsn,sizeof(rsn));memset(c.pmk,0x31,sizeof(c.pmk));
    assert(aic8800_security_init(&security));assert(aic8800_supplicant_init(&supplicant,&c));
    assert(aic8800_transport_start_security(&transport,&security,&supplicant,m.now+50000));assert(supplicant.rsn);
    security.authorized=security.port_confirmed=1;security.keys[0].valid=1;
    security.keys[0].pairwise=1;security.keys[0].generation=1;
    supplicant.state=AIC8800_SUPPLICANT_AUTHORIZED;wifi.link=AIC8800_WIFI_SECURED;
}
static void dispatcher_step(void)
{aic8800_dispatch_poll(&bootstrap_dispatch);vTaskDelay(1);}
static void scan_application(void)
{
    assert(aic8800_dispatch_init(&bootstrap_dispatch,&transport));
    assert(!aic8800_dispatch_init(&bootstrap_dispatch,&transport));
    uint64_t deadline=m.now+(mode==2?400:3000);
    assert(aic8800_transport_scan(&transport,deadline));
    int identity_checked=0;
    for(unsigned i=0;i<3500;++i){
        const uint8_t *record;size_t available;
        if(!identity_checked&&aic8800_transport_receive_view(&transport,&bootstrap_dispatch,&record,&available)){
            assert(available>=16);unsigned before=m.accesses;
            aic8800_transport_poll(&transport);assert(m.accesses==before);
            assert(!aic8800_transport_retire_record(&transport,&net,record,16));
            assert(!aic8800_transport_retire_record(&transport,&bootstrap_dispatch,record+4,16));
            assert(!aic8800_transport_retire_record(&transport,&bootstrap_dispatch,record,1));identity_checked=1;
        }
        dispatcher_step();
        if(bootstrap_dispatch.faulted||(bootstrap_dispatch.event_count==4&&transport.held))break;
    }
    assert(identity_checked&&scan_commands==1);
    if(mode>=3){
        assert(bootstrap_dispatch.faulted&&transport.state==AIC8800_TRANSPORT_FAULT&&!wifi.summary.accepted);
        assert(!bootstrap_dispatch.retired_records&&transport.cursor<transport.rx_bytes);
        unsigned accesses=m.accesses;dispatcher_step();assert(accesses==m.accesses);
        platform_puts("PASS dispatcher: ambiguous or active malformed confirmation retains terminal ownership\n");return;
    }
    assert(!bootstrap_dispatch.faulted&&bootstrap_dispatch.event_count==4&&wifi.held&&transport.held);
    assert(!aic8800_transport_consume(&transport));
    if(mode==2){
        while(!bootstrap_dispatch.faulted)dispatcher_step();
        assert(transport.state==AIC8800_TRANSPORT_FAULT&&transport.held&&!wifi.summary.accepted);
    }
    uint64_t last=0;
    for(unsigned i=0;i<5;++i){
        struct aic8800_dispatch_event e;
        for(unsigned j=0;j<100&&!bootstrap_dispatch.event_count;++j)dispatcher_step();
        assert(aic8800_dispatch_take_event(&bootstrap_dispatch,&e));
        assert(e.kind==AIC8800_DISPATCH_SCAN&&e.sequence>last&&e.bytes==41&&!e.metadata.scan.frame);
        assert(e.payload[40]==(uint8_t)('0'+i));last=e.sequence;
        if(mode!=2)for(unsigned j=0;j<4;++j)dispatcher_step();
    }
    if(mode==2){
        assert(transport.held&&!bootstrap_dispatch.event_count&&bootstrap_dispatch.terminal_event_record);
        unsigned accesses=m.accesses;dispatcher_step();assert(accesses==m.accesses);
        platform_puts("PASS dispatcher: event backpressure deadline retains raw and ordered copied events\n");return;
    }
    for(unsigned i=0;i<200&&wifi.state!=AIC8800_WIFI_SCAN_DONE;++i)dispatcher_step();
    assert(wifi.state==AIC8800_WIFI_SCAN_DONE&&wifi.summary.accepted&&wifi.summary.received==5);
    assert(bootstrap_dispatch.retired_records==5&&bootstrap_dispatch.discarded_diagnostics==1);
    assert(bootstrap_dispatch.record_count==4&&!bootstrap_dispatch.event_count);
    platform_puts("PASS dispatcher: foreign and stale events retire before actual scan completion; FIFO backpressure preserves results\n");
}
static void dispatch_application(void *context)
{
    (void)context;if(mode){scan_application();platform_exit(0);}
    assert(sunstone_net_activate(&net));
    for(unsigned i=0;i<100&&!aic8800_network_ready(&net.network);++i)vTaskDelay(1);
    assert(aic8800_network_ready(&net.network)&&net.network.receive==transport.receive_owner);
    uint64_t udp=open_socket(0,7000);
    struct sunstone_net_request r={.operation=SUNSTONE_NET_SEND,.handle=udp,.port=9000,.timeout=1000,.bytes=1};
    memcpy(r.address,peer_ip,4);assert(call(&r).result==SUNSTONE_NET_OK);
    for(unsigned i=0;i<1000&&!net.network.rx_packets;++i)vTaskDelay(1);assert(net.network.rx_packets);vTaskDelay(2);
    exchange(udp,"dispatcher UDP");
    assert(udp_packets==1&&eapol_records>=2&&supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED);
    assert(net.network.receive->record_count==4&&net.network.receive->discarded_diagnostics>=16);
    assert(!net.network.receive->event_count&&!transport.held);
    /* Wait for a clean receive boundary, then deliver a real source-layout
     * disconnect indication and retrieve its copied event after revocation. */
    for(unsigned i=0;i<1000&&(transport.pending||m.card[1][4]||transport.cursor!=transport.rx_bytes);++i)vTaskDelay(1);
    assert(!transport.pending&&!m.card[1][4]&&transport.cursor==transport.rx_bytes);
    memset(receive,0,sizeof(receive));const uint8_t loss[]={3,0,1,0,0,0};control(receive,0x1805,loss,6);m.card[1][4]=1;
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_EVENT,.timeout=1000};struct sunstone_net_reply result=call(&r);
    assert(result.result==SUNSTONE_NET_OK&&result.event.kind==AIC8800_DISPATCH_STATION&&
        result.event.generation==1&&result.event.metadata.station.kind==AIC8800_WIFI_DISCONNECTION&&!result.event.metadata.station.payload);
    assert(!aic8800_network_ready(&net.network));
    platform_puts("PASS dispatcher: sustained actual UDP/RSN receive survives unrelated traffic; copied disconnect revokes network\n");platform_exit(0);
}
static void dispatch_arguments(void)
{
    char command[128]={0};uint64_t block[2]={(uintptr_t)command,sizeof(command)};
    register uint64_t op __asm__("x0")=0x15;register uint64_t *arg __asm__("x1")=block;
    __asm__ volatile("hlt #0xf000":"+r"(op):"r"(arg):"memory");
    if(!op)for(unsigned i=0;i+9<sizeof(command);++i){
        if(!memcmp(command+i,"events",6))mode=1;
        if(!memcmp(command+i,"deadline",8))mode=2;
        if(!memcmp(command+i,"cfm",3))mode=3;
        if(!memcmp(command+i,"ambiguous",9))mode=4;
    }
}
void kernel_main(void)
{
    platform_init();dispatch_arguments();dispatch_checkpoint();
    TaskHandle_t app=xTaskCreateStatic(dispatch_application,"dispatch-app",4096,NULL,5,app_stack,&app_task);assert(app);
    if(!mode){
        struct sunstone_net_config c={.network={.transport=&transport,.tx_timeout=5000,.queue_timeout=5000,
            .random32=random32,.sequence=sequence,.entropy_context=&m,.entropy_record=1,
            .address={10,0,0,2},.netmask={255,255,255,0},.gateway={10,0,0,1}},.bootstrap=app,.clients={app}};
        assert(sunstone_net_init(&net,&c));
    }
    vTaskStartScheduler();platform_panic("scheduler",0);
}
