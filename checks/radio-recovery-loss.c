/* Clean unsolicited link loss and recovery in the actual retained session:
 * an idle fixed-address OPEN association receives a disconnection indication,
 * goes offline without fault, keeps its socket, retries a failed
 * reassociation, and restores the same IP interface. Lower firmware, initial
 * association and RF remain modeled, as in the reconnect composition. */
#define kernel_main prior_session_kernel_main
#include "a733-radio-session.c"
#undef kernel_main
static atomic_uint loss_test;
static unsigned connect_commands,lost_polls,drained_events;
static uint64_t kept_handle;
static unsigned loss_wire(uint8_t *p,uint16_t id,const uint8_t *payload,unsigned n)
{
    unsigned size=12+n;p[0]=(uint8_t)size;p[1]=(uint8_t)(size>>8);p[2]=0x11;
    p[4]=(uint8_t)id;p[5]=(uint8_t)(id>>8);p[10]=(uint8_t)n;p[11]=(uint8_t)(n>>8);
    memcpy(p+16,payload,n);return (16+n+3)&~3u;
}
static int loss_mmio_write(void *ctx,uintptr_t address,uint32_t value)
{
    if(address<0x04021000||address>0x04021300||session.transport.state!=AIC8800_TRANSPORT_RUNTIME)
        return session_write(ctx,address,value);
    host_accessible(ctx);uint32_t offset=(uint32_t)(address-0x04021000);
    int ok=write_cb(&m,offset,value);
    if(offset!=0x200||!m.write||m.position!=m.bytes)return ok;
    if(m.transmitted[2]==1){
        unsigned n=m.transmitted[4]|(unsigned)m.transmitted[5]<<8;assert(n<=1500&&n+32<=m.bytes);
        uint8_t ethernet[1514];memcpy(ethernet,m.transmitted+12,14);memcpy(ethernet+14,m.transmitted+32,n);peer(ethernet,n+14);return ok;
    }
    /* Only reassociation reaches the firmware: no disconnect, port or key command. */
    assert(m.transmitted[2]==0x11&&m.bytes==512);unsigned id=m.transmitted[8]|(unsigned)m.transmitted[9]<<8;
    assert(id==0x1800&&atomic_load(&loss_test)==5);
    assert(atomic_load(&session.sockets.network.offline_ack)==atomic_load(&session.sockets.network.offline_request));
    memset(receive,0,sizeof(receive));
    /* The first attempt fails in the indication; the retry associates. */
    uint8_t accepted=0,ind[852]={0};memcpy(ind+2,peer_mac,6);ind[9]=1;ind[10]=3;ind[824]=0x6c;ind[825]=9;
    if(++connect_commands==1)ind[0]=1;
    unsigned at=loss_wire(receive,0x1801,&accepted,1);at+=loss_wire(receive+at,0x1802,ind,sizeof(ind));
    m.card[1][4]=(uint8_t)((at+511)/512);return ok;
}
static void loss_model_setup(void)
{
    memset(&r,0,sizeof(r));pmic_model_setup(&r.power);memset(&m,0,sizeof(m));
    *pmic_model_word(&r.power,0x02002000)=0xf8000000u|(99u<<16)|(99u<<8);
    *pmic_model_word(&r.power,0x02002580)=1u<<31;*pmic_model_word(&r.power,0x02002588)=1u<<31;
    *pmic_model_word(&r.power,0x020025c0)=1u<<24;*pmic_model_word(&r.power,0x020025e0)=1u<<30;
    *pmic_model_word(&r.power,0x0200259c)=0x30001;
    m.regs[0x300/4]=0x03050530;m.regs[0x5c/4]=0x81710000;
    cis(0,1);cis(1,0);cis(2,1);m.card[0][0]=0x43;m.card[0][1]=3;m.card[0][8]=2;
    m.card[0][2]=4;m.card[0][3]=4;m.card[0][4]=0x80;m.card[1][0]=0x40;m.card[0][0x110]=64;
    struct a733_radio_platform_config p={.owner=42,.handoff=200,.deadline_us=50000000,.lifetime_deadline_us=120000000,
        .topology={201,A733_RADIO_DCDC1,A733_RADIO_DCDC8,A733_RADIO_DCDC8,A733_RADIO_DCDC8},
        .fabric={0,1u<<31,1u<<31},.fabric_hz={24000000,24000000,24000000},
        .card={.supported_ocr=0x00300000,.command_timeout=20000,.ready_poll_interval=1000}};
    for(unsigned i=0;i<8;++i)p.desired[i].function=i<6?2:i==6?0:1;
    struct a733_radio_platform_ops ops={session_now,session_current,radio_admit,radio_enter,radio_leave,radio_retain,session_read,loss_mmio_write,radio_barrier};
    assert(a733_radio_platform_init(&r.radio,&p,&ops,&r,&r.power.clocks,&r.power.pins,&r.power.rails,&r.power.provider));
    struct a733_radio_session_config c={.owner_task=session_task,.owner=80,.bootstrap_deadline=60000000,
        .runtime_lease_us=10000000,.renew_margin_us=3000000,
        .transport={.variant=AIC8800_TRANSPORT_D80_BOOTSTRAP,.pad=AIC8800_PAD_KEEP,.ticks_per_ms=1000,.command_timeout=100000,.poll_interval=1000},
        .bootstrap={1,0x40500058,0x40,1,1},
        .firmware={.identity={.chip_word=0x00c30000,.subrevision_word=0x70},.profile=1,
            .blobs={{session_blob0,1708},{session_blob1,24744},{session_blob2,23288},{session_blob3,341260}}},
        .runtime={model_id,&m,1000000},
        .wifi={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=1000000,.profile=1,.channel_count=1,.channels={{2412,0,1,17}}}};
    assert(a733_radio_session_init(&session,&r.radio,&c));
}
/* WPA2 retirement after loss: no port command, the pairwise key is dropped
 * locally and only the group key is deleted. Rearm accepts any later
 * association, as after a retried reassociation. Keys are modeled inputs. */
static int loss_entropy(void *ctx,uint8_t *bytes,size_t n)
{assert(ctx==&m);memset(bytes,0x5a,n);return 1;}
static void lost_retirement(void)
{
    static struct aic8800_sdio_transport t;
    static struct aic8800_wifi w;
    static struct aic8800_data d;
    static struct aic8800_security sec;
    static struct aic8800_supplicant rsn;
    t=session.transport;w=session.wifi;d=session.data;
    t.wifi=&w;t.data=&d;t.security=NULL;t.supplicant=NULL;d.security=NULL;d.owner=&t;d.wifi=&w;d.next_sequence=73;
    w.owner=&t;w.association_generation=1;w.link=AIC8800_WIFI_SECURITY_PENDING;w.target.security=AIC8800_WIFI_WPA2_PSK_CCMP;w.target.beacon_evidence=17;
    struct aic8800_supplicant_config cfg={.entropy=loss_entropy,.entropy_context=&m,.entropy_record=7,.beacon_record=17,
        .ticks_per_second=1000000,.command_timeout=100000,.ap_rsn_bytes=22,
        .ap_rsn={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0}};
    memset(cfg.pmk,0x71,32);memset(&sec,0,sizeof(sec));memset(&rsn,0,sizeof(rsn));
    assert(aic8800_security_init(&sec)&&aic8800_supplicant_init(&rsn,&cfg));
    uint64_t now=r.power.now,deadline=now+1000000;
    assert(aic8800_supplicant_claim(&rsn,&t,&sec,now,deadline)&&rsn.rsn);
    t.security=&sec;t.supplicant=&rsn;d.security=&sec;
    sec.keys[0]=(struct aic8800_security_key){.generation=1,.valid=1,.pairwise=1,.hardware_index=8};
    sec.keys[2]=(struct aic8800_security_key){.generation=1,.valid=1,.key_id=1,.hardware_index=9};
    sec.port_confirmed=sec.authorized=1;rsn.state=AIC8800_SUPPLICANT_AUTHORIZED;w.link=AIC8800_WIFI_SECURED;
    w.link=AIC8800_WIFI_LINK_UNRESOLVED;assert(!aic8800_supplicant_retire(&rsn,now,deadline));
    w.link=AIC8800_WIFI_LINK_DOWN;aic8800_supplicant_observe(&rsn,++now);
    assert(rsn.state==AIC8800_SUPPLICANT_AUTHORIZED&&!sec.authorized);
    assert(aic8800_supplicant_retire(&rsn,now,deadline));
    assert(!sec.keys[0].valid&&!sec.port_confirmed&&rsn.retire_closed&&sec.keys[2].valid);
    aic8800_supplicant_step(&rsn,++now);
    assert(sec.state==AIC8800_SECURITY_REQUEST&&sec.operation==AIC8800_SECURITY_DELETE&&sec.slot==2&&sec.request[16]==9);
    assert(aic8800_security_sent(&sec,&t));
    struct aic8800_sdio_response response={.has_message=1,.message_id=39};
    assert(aic8800_security_response(&sec,&t,&response,++now)==AIC8800_CODEC_OK);
    aic8800_supplicant_step(&rsn,++now);aic8800_supplicant_step(&rsn,++now);
    assert(rsn.state==AIC8800_SUPPLICANT_RETIRED&&!rsn.rsn&&sec.retired&&!sec.keys[2].valid);
    assert(aic8800_transport_finish_security(&t));
    assert(!aic8800_supplicant_rearm(&rsn,&sec,&t,&cfg)); /* Same association. */
    w.association_generation=3;assert(aic8800_supplicant_rearm(&rsn,&sec,&t,&cfg)&&d.next_sequence==73);
}
static void loss_owner(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<20000;++i){
        r.power.now=100+(uint64_t)xTaskGetTickCount()*1000;m.now=r.power.now;
        assert(pmic_model_step(&r.power)!=A733_PMIC_RETAINED);open_rx();
        unsigned pins=r.radio.pin_lease.id!=0,enabled=*pmic_model_word(&r.power,0x07025040)&2;
        enum a733_radio_session_result status=a733_radio_session_poll(&session);
        if(!pins&&r.radio.pin_lease.id)r.pins_at=r.power.now;
        if(!enabled&&(*pmic_model_word(&r.power,0x07025040)&2))r.enabled_at=r.power.now;
        assert(status!=A733_SESSION_INVALID&&status!=A733_SESSION_RETAINED);
        adopted_checkpoints();
        if(association_checkpoint&&!session.wifi.target.ssid_bytes){
            session.wifi.target.ssid_bytes=1;session.wifi.target.ssid[0]='x';
            session.wifi.target.connection_authority=10;session.wifi.target.beacon_evidence=11;
        }
        if(status==A733_SESSION_MANAGEMENT&&!atomic_load(&network_started)&&a733_radio_session_authorize_open(&session,91)){
            struct sunstone_net_config c={.network={.transport=&session.transport,.dispatcher=&session.dispatcher,
                .address={10,0,0,2},.netmask={255,255,255,0},.gateway={10,0,0,1},.tx_timeout=2000000,.queue_timeout=3000,
                .random32=model_entropy,.sequence=model_sequence,.entropy_context=&m,.entropy_record=1},
                .bootstrap=session_task,.clients={client_task}};
            assert(a733_radio_session_start_network(&session,&c));lost_retirement();atomic_store(&network_started,1);
        }
        struct aic8800_dispatch_event event;
        if(atomic_load(&loss_test)>=3&&a733_radio_session_take_event(&session,&event))++drained_events;
        unsigned phase=atomic_load(&loss_test);
        struct aic8800_sdio_transport *t=&session.transport;
        if(phase==2&&!m.card[1][4]&&!t->pending&&!t->held&&t->cursor==t->rx_bytes){
            memset(receive,0,sizeof(receive));const uint8_t loss[6]={3,0,1,0,0,0};
            m.card[1][4]=(uint8_t)((loss_wire(receive,0x1805,loss,6)+511)/512);atomic_store(&loss_test,3);
        }else if(phase==3&&status==A733_SESSION_LOST){
            /* Offline without fault; admission closed; the socket remains. */
            assert(!atomic_load(&session.sockets.network.failed)&&!session.fault&&atomic_load(&session.sockets.offline));
            assert(session.wifi.link==AIC8800_WIFI_LINK_DOWN&&!aic8800_network_ready(&session.sockets.network));
            assert(!session.data.open_authority&&!connect_commands);
            if(++lost_polls==3)atomic_store(&loss_test,4);
        }else if(phase==4){
            struct aic8800_wifi_target target=session.wifi.target,foreign=target;foreign.bssid[5]^=1;
            assert(!a733_radio_session_recover(&session,&foreign,NULL,92,session.deadline));
            assert(!a733_radio_session_recover(&session,&target,NULL,0,session.deadline));
            assert(!a733_radio_session_reconnect(&session,&target,NULL,92,3,session.deadline));
            assert(a733_radio_session_recover(&session,&target,NULL,92,session.deadline));
            assert(!a733_radio_session_recover(&session,&target,NULL,92,session.deadline));
            atomic_store(&loss_test,5);
        }else if(phase==5&&status==A733_SESSION_NETWORK&&!atomic_load(&session.sockets.offline)){
            assert(connect_commands==2&&session.wifi.association_generation==3&&session.sockets.network.generation==3);
            assert(!aic8800_network_lost(&session.sockets.network)&&!atomic_load(&session.sockets.network.failed));
            assert(drained_events>=3&&!session.fault&&session.data.open_authority==92);
            atomic_store(&loss_test,6);
        }
        if(atomic_load(&loss_test)==7){
            assert(tcp_packets==1&&session.borrow.generation==r.radio.clock.id);
            platform_puts("PASS clean link loss: offline without fault, socket kept, retried reassociation, restored interface and new TCP exchange; lower firmware/initial association and RF remain modeled\n");
            platform_exit(0);
        }
        vTaskDelay(1);
    }
    assert(0);
}
static void loss_client(void *unused)
{
    (void)unused;while(!atomic_load(&network_started))vTaskDelay(1);
    struct sunstone_net_request r={.operation=SUNSTONE_NET_OPEN_UDP,.port=7400,.timeout=5000};
    struct sunstone_net_reply reply=request(&r);assert(reply.result==SUNSTONE_NET_OK&&reply.handle);kept_handle=reply.handle;
    atomic_store(&loss_test,2);while(atomic_load(&loss_test)<4)vTaskDelay(1);
    uint64_t ticket;r=(struct sunstone_net_request){.operation=SUNSTONE_NET_OPEN_TCP,.timeout=5000};
    assert(!sunstone_net_submit(&session.sockets,&r,&ticket)); /* Admission closed while lost. */
    while(atomic_load(&loss_test)!=6)vTaskDelay(1);
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CLOSE,.handle=kept_handle,.timeout=5000};assert(request(&r).result==SUNSTONE_NET_OK);
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_OPEN_TCP,.port=7401,.timeout=5000};reply=request(&r);assert(reply.result==SUNSTONE_NET_OK&&reply.handle>kept_handle);uint64_t handle=reply.handle;
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CONNECT,.handle=handle,.port=9000,.timeout=5000};memcpy(r.address,peer_ip,4);assert(request(&r).result==SUNSTONE_NET_OK);
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_SEND,.handle=handle,.bytes=1,.timeout=5000};r.payload[0]='l';assert(request(&r).result==SUNSTONE_NET_OK);
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_RECEIVE,.handle=handle,.bytes=1,.timeout=5000};reply=request(&r);assert(reply.result==SUNSTONE_NET_OK&&reply.bytes==1&&reply.payload[0]=='l');
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CLOSE,.handle=handle,.timeout=5000};assert(request(&r).result==SUNSTONE_NET_OK);atomic_store(&loss_test,7);vTaskSuspend(NULL);
}
void kernel_main(void)
{
    platform_init();session_task=xTaskCreateStatic(loss_owner,"loss",8192,NULL,4,session_stack,&session_task_storage);assert(session_task);
    client_task=xTaskCreateStatic(loss_client,"client",4096,NULL,5,client_stack,&client_task_storage);assert(client_task);
    loss_model_setup();vTaskStartScheduler();assert(0);
}
