/* Additional integration only. Initial firmware/association/security/application
 * metadata are explicit checkpoints; reconnect and its new handshake execute. */
#include "../apps/board/radio.h"
static struct board_radio wpa_board;
#define SUNSTONE_RADIO_SESSION_OBJECT wpa_board.session
#define kernel_main prior_session_kernel_main
#include "a733-radio-session.c"
#undef kernel_main
#undef session
extern const uint8_t radio_wpa_vectors[],radio_wpa_vectors_end[];
atomic_uint reconnect_checkpoint_gate;
uint64_t reconnect_checkpoint_view[8];
static atomic_uint wpa_result;
static atomic_uint recovery_state;
static uint8_t vector[15][256];static unsigned vector_size[15];
static volatile unsigned entropy_samples,entropy_before,old_deletes,old_closed,new_keys,new_open,eapol_sent,ap_stage,connects,disconnects;
static uint64_t receive_pn;
static int wpa_entropy(void *ctx,uint8_t *p,size_t bytes)
{assert(ctx==&m&&bytes==32);++entropy_samples;for(unsigned i=0;i<bytes;++i)p[i]=(uint8_t)(128+i);return 1;}
static unsigned wpa_wire(uint8_t *p,uint16_t id,const uint8_t *params,unsigned n)
{
    unsigned size=12+n;p[0]=(uint8_t)size;p[1]=(uint8_t)(size>>8);p[2]=0x11;
    p[4]=(uint8_t)id;p[5]=(uint8_t)(id>>8);p[10]=(uint8_t)n;p[11]=(uint8_t)(n>>8);
    memcpy(p+16,params,n);return (16+n+3)&~3u;
}
static void wpa_confirmation(uint16_t id,const uint8_t *p,unsigned n)
{memset(receive,0,sizeof(receive));unsigned bytes=wpa_wire(receive,id,p,n);m.card[1][4]=(uint8_t)((bytes+511)/512);}
static int wpa_mmio_write(void *ctx,uintptr_t address,uint32_t value)
{
    struct a733_radio_session *s=&wpa_board.session;
    if(address<0x04021000||address>0x04021300||s->transport.state!=AIC8800_TRANSPORT_RUNTIME)
        return session_write(ctx,address,value);
    host_accessible(ctx);uint32_t offset=(uint32_t)(address-0x04021000);int ok=write_cb(&m,offset,value);
    if(offset!=0x200||!m.write||m.position!=m.bytes)return ok;
    const uint8_t *p=m.transmitted;
    if(p[2]==1){
        if(s->data.result.eapol){
            unsigned index=eapol_sent?8:6;assert(eapol_sent<2&&vector_size[index]+32<=m.bytes);
            assert((p[4]|(unsigned)p[5]<<8)==vector_size[index]);
            assert(!memcmp(p+32,vector[index],vector_size[index]));++eapol_sent;
            memset(receive,0,sizeof(receive));receive[0]=8;receive[2]=0x92;put32(receive+4,9);put32(receive+8,get32(p+8));m.card[1][4]=1;
        }else{
            assert(aic8800_security_open(&s->security));unsigned n=p[4]|(unsigned)p[5]<<8;
            assert(n<=1500&&n+32<=m.bytes);uint8_t ethernet[1514];memcpy(ethernet,p+12,14);memcpy(ethernet+14,p+32,n);peer(ethernet,n+14);
        }
        return ok;
    }
    assert(p[2]==0x11&&m.bytes==512);unsigned id=p[8]|(unsigned)p[9]<<8;
    if(id==5124){
        assert(p[14]==2&&p[16]==3);
        if(!p[17]){
            assert(!old_closed&&!disconnects&&s->wifi.link==AIC8800_WIFI_SECURED);
            assert(atomic_load(&s->sockets.network.offline_ack)==atomic_load(&s->sockets.network.offline_request));
            assert(!atomic_load(&s->sockets.network.address_ready)&&!s->sockets.network.endpoint.bits.bEndPointUp);++old_closed;
        }else{assert(old_closed&&old_deletes==2&&connects==1&&new_keys==2&&eapol_sent==2);++new_open;}
        const uint8_t opaque[]={0x12,0x34};wpa_confirmation(5125,opaque,2);
    }else if(id==38){
        assert(old_closed&&!disconnects&&old_deletes<2&&p[16]==11+old_deletes);++old_deletes;
        const uint8_t opaque[]={0xa5,0x5a};wpa_confirmation(39,opaque,2);
    }else if(id==36){
        assert(old_deletes==2&&connects==1&&p[14]==44&&p[20]==16&&p[56]==2&&p[57]==1);
        unsigned which=p[59]?1:2;assert(!memcmp(p+24,vector[which],16));++new_keys;
        const uint8_t accepted[]={0,(uint8_t)(10+which),0xa5,0x5a};wpa_confirmation(37,accepted,4);
    }else if(id==0x1803){
        assert(old_closed&&old_deletes==2&&!s->transport.security&&!s->transport.supplicant&&
            s->supplicant.state==AIC8800_SUPPLICANT_RETIRED&&!s->supplicant.rsn);
        assert(!disconnects&&p[16]==3&&p[18]==1);++disconnects;
        memset(receive,0,sizeof(receive));uint8_t opaque[2]={0xa5,0x5a},loss[6]={3,0,1,0,0,0};
        unsigned at=wpa_wire(receive,0x1804,opaque,2);at+=wpa_wire(receive+at,0x1805,loss,6);m.card[1][4]=(uint8_t)((at+511)/512);
    }else{
        assert(id==0x1800&&disconnects==1&&!connects);++connects;
        memset(receive,0,sizeof(receive));uint8_t accepted=0,ind[852]={0};memcpy(ind+2,peer_mac,6);ind[9]=1;ind[10]=3;ind[824]=0x6c;ind[825]=9;
        unsigned at=wpa_wire(receive,0x1801,&accepted,1);at+=wpa_wire(receive+at,0x1802,ind,sizeof(ind));m.card[1][4]=(uint8_t)((at+511)/512);
    }
    return ok;
}
static void wpa_receive(const uint8_t *ethernet,unsigned n,int encrypted)
{
    memset(receive,0,sizeof(receive));unsigned bytes=24+(encrypted?8:0)+8+n-14;
    receive[0]=(uint8_t)bytes;receive[1]=(uint8_t)(bytes>>8);receive[48]=0x40;receive[49]=1;receive[50]=3;receive[51]=0xff;
    receive[60]=8;receive[61]=encrypted?0x42:2;memcpy(receive+64,ethernet,6);memcpy(receive+70,peer_mac,6);memcpy(receive+76,ethernet+6,6);
    unsigned offset=84;
    if(encrypted){receive[36]=12;uint64_t pn=++receive_pn;receive[84]=(uint8_t)pn;receive[85]=(uint8_t)(pn>>8);receive[87]=0x20;
        for(unsigned i=0;i<4;++i)receive[88+i]=(uint8_t)(pn>>(16+8*i));offset+=8;}
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0};memcpy(receive+offset,snap,6);memcpy(receive+offset+6,ethernet+12,n-12);
    m.card[1][4]=(uint8_t)((60+bytes+511)/512);
}
static void wpa_peer_step(void)
{
    struct a733_radio_session *s=&wpa_board.session;struct aic8800_sdio_transport *t=&s->transport;
    if(m.card[1][4]||t->pending||t->held||t->cursor!=t->rx_bytes)return;
    if(s->supplicant.generation==2&&s->supplicant.state==AIC8800_SUPPLICANT_HANDSHAKE&&
       !s->supplicant.count&&!s->supplicant.action_active&&!s->supplicant.port_pending&&s->data.state==AIC8800_DATA_IDLE&&
       ((!ap_stage&&!eapol_sent)||(ap_stage==1&&eapol_sent==1))){
        unsigned index=ap_stage?7:5;uint8_t ethernet[270];memcpy(ethernet,local_mac,6);memcpy(ethernet+6,peer_mac,6);
        ethernet[12]=0x88;ethernet[13]=0x8e;memcpy(ethernet+14,vector[index],vector_size[index]);wpa_receive(ethernet,14+vector_size[index],0);++ap_stage;return;
    }
    if(peer_count&&aic8800_security_open(&s->security)){
        wpa_receive(peer_rx[peer_head].bytes,peer_rx[peer_head].length,1);peer_head=(peer_head+1)%8;--peer_count;
    }
}
static void wpa_model_setup(void)
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
    struct a733_radio_platform_ops ops={session_now,session_current,radio_admit,radio_enter,radio_leave,radio_retain,session_read,wpa_mmio_write,radio_barrier};
    assert(a733_radio_platform_init(&r.radio,&p,&ops,&r,&r.power.clocks,&r.power.pins,&r.power.rails,&r.power.provider));
    struct a733_radio_session_config c={.owner_task=session_task,.owner=80,.bootstrap_deadline=60000000,
        .runtime_lease_us=10000000,.renew_margin_us=3000000,
        .transport={.variant=AIC8800_TRANSPORT_D80_BOOTSTRAP,.pad=AIC8800_PAD_KEEP,.ticks_per_ms=1000,.command_timeout=100000,.poll_interval=1000},
        .bootstrap={1,0x40500058,0x40,1,1},
        .firmware={.identity={.chip_word=0x00c30000,.subrevision_word=0x70},.profile=1,
            .blobs={{session_blob0,1708},{session_blob1,24744},{session_blob2,23288},{session_blob3,341260}}},
        .runtime={model_id,&m,1000000},
        .wifi={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=1000000,.profile=1,.channel_count=1,.channels={{2412,0,1,17}}}};
    assert(a733_radio_session_init(&wpa_board.session,&r.radio,&c));
}
static void wpa_profile(void)
{
    const uint8_t *p=radio_wpa_vectors;
    for(unsigned i=0;i<11;++i){assert(p+2<=radio_wpa_vectors_end);unsigned n=p[0]|(unsigned)p[1]<<8;p+=2;
        assert(n<=256&&p+n<=radio_wpa_vectors_end);vector_size[i]=n;memcpy(vector[i],p,n);p+=n;}
    assert(p==radio_wpa_vectors_end&&vector_size[0]==32&&vector_size[4]==22);
    struct aic8800_supplicant_config *cfg=&wpa_board.config.security;
    *cfg=(struct aic8800_supplicant_config){.entropy=wpa_entropy,.entropy_context=&m,.entropy_record=7,.beacon_record=17,
        .ticks_per_second=1000000,.command_timeout=500000,.ap_rsn_bytes=22};
    memcpy(cfg->pmk,vector[0],32);memcpy(cfg->ap_rsn,vector[4],22);
}
static void wpa_owner(void *unused)
{
    (void)unused;unsigned initial_security=0,checkpoint=0,requested=0;
    for(unsigned i=0;i<20000;++i){
        struct a733_radio_session *s=&wpa_board.session;
        if(checkpoint&&!atomic_load(&reconnect_checkpoint_gate)){vTaskDelay(1);continue;}
        r.power.now=100+(uint64_t)xTaskGetTickCount()*1000;m.now=r.power.now;
        /* Request before polling can start another SDIO operation. The saved
         * checkpoint is drained; a refusal here is an immediate fixture fault. */
        if(checkpoint&&!requested){
            assert(aic8800_transport_attachment_ready(&s->transport));
            assert(board_radio_reconnect(&wpa_board,s->deadline));requested=1;
        }
        assert(pmic_model_step(&r.power)!=A733_PMIC_RETAINED);wpa_peer_step();
        unsigned pins=r.radio.pin_lease.id!=0,enabled=*pmic_model_word(&r.power,0x07025040)&2;
        enum a733_radio_session_result status=a733_radio_session_poll(s);
        if(!pins&&r.radio.pin_lease.id)r.pins_at=r.power.now;
        if(!enabled&&(*pmic_model_word(&r.power,0x07025040)&2))r.enabled_at=r.power.now;
        assert(status!=A733_SESSION_INVALID&&status!=A733_SESSION_RETAINED);
        adopted_checkpoints();
        if(association_checkpoint&&!s->wifi.target.ssid_bytes){
            s->wifi.target.ssid_bytes=1;s->wifi.target.ssid[0]='w';s->wifi.target.connection_authority=10;s->wifi.target.beacon_evidence=17;
            s->wifi.target.security=AIC8800_WIFI_WPA2_PSK_CCMP;s->wifi.link=AIC8800_WIFI_SECURITY_PENDING;
            wpa_board.config.target=s->wifi.target;
        }
        if(status==A733_SESSION_MANAGEMENT&&!initial_security&&
           a733_radio_session_start_security(s,&wpa_board.config.security,s->deadline)){
            /* Reuse the completed initial-security boundary; do not repeat the
             * accepted handshake/crypto cases. The actual old RSN allocation
             * exists so normal retirement must deinitialize it correctly. */
            assert(s->supplicant.rsn&&!s->supplicant.count&&!s->supplicant.action_active&&s->supplicant.generation==1);
            s->security.keys[0]=(struct aic8800_security_key){.generation=1,.valid=1,.pairwise=1,.hardware_index=11};
            s->security.keys[2]=(struct aic8800_security_key){.generation=1,.valid=1,.key_id=1,.hardware_index=12};
            memcpy(s->security.keys[0].material,vector[1],16);memcpy(s->security.keys[2].material,vector[2],16);
            s->security.authorized=1;s->security.port_confirmed=1;s->supplicant.state=AIC8800_SUPPLICANT_AUTHORIZED;
            s->supplicant.completed=1;s->wifi.link=AIC8800_WIFI_SECURED;initial_security=1;
        }
        if(status==A733_SESSION_MANAGEMENT&&initial_security&&!atomic_load(&network_started)){
            struct sunstone_net_config c={.network={.transport=&s->transport,.dispatcher=&s->dispatcher,
                .address={10,0,0,2},.netmask={255,255,255,0},.gateway={10,0,0,1},.tx_timeout=2000000,.queue_timeout=3000,
                .random32=model_entropy,.sequence=model_sequence,.entropy_context=&m,.entropy_record=1},
                .bootstrap=session_task,.clients={client_task}};
            if(a733_radio_session_start_network(s,&c)){
                /* Board application ONLINE metadata is an explicit checkpoint;
                 * shared entropy/board startup were qualified in earlier cases. */
                wpa_board.self=&wpa_board;wpa_board.constructed=1;wpa_board.stage=3; /* radio.c ONLINE */
                wpa_board.config.session.owner_task=session_task;wpa_board.config.recovery_state=&recovery_state;
                atomic_store(&wpa_board.network_started,1);atomic_store(&network_started,1);
            }
        }
        if(!checkpoint&&atomic_load(&network_started)&&aic8800_network_address(&s->sockets.network,NULL)&&
           aic8800_transport_attachment_ready(&s->transport)){
            entropy_before=entropy_samples;checkpoint=1;
            reconnect_checkpoint_view[0]=s->wifi.association_generation;
            reconnect_checkpoint_view[1]=s->security.authorized;
            reconnect_checkpoint_view[2]=s->supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED;
            reconnect_checkpoint_view[3]=aic8800_network_address(&s->sockets.network,NULL);
            reconnect_checkpoint_view[4]=s->borrow.generation;reconnect_checkpoint_view[5]=r.radio.clock.id;
            reconnect_checkpoint_view[6]=old_deletes;reconnect_checkpoint_view[7]=disconnects;
            platform_puts("CHECKPOINT WPA2 owner before reconnect\n");
        }
        if(requested&&!atomic_load(&wpa_result)&&status==A733_SESSION_NETWORK&&!atomic_load(&s->sockets.offline)&&s->wifi.association_generation==2){
            assert(old_closed==1&&old_deletes==2&&disconnects==1&&connects==1&&eapol_sent==2&&new_keys==2&&new_open==1);
            assert(entropy_samples==entropy_before+1&&s->supplicant.generation==2&&s->supplicant.state==AIC8800_SUPPLICANT_AUTHORIZED);
            assert(s->data.next_sequence>=2&&aic8800_security_open(&s->security)&&s->sockets.network.generation==2);
            atomic_store(&wpa_result,1);
        }
        if(atomic_load(&wpa_result)==2){
            assert(syn_packets==1&&tcp_packets==1&&s->borrow.generation==r.radio.clock.id&&!s->fault);
            platform_puts("PASS WPA2 board owner reconnect: processed IP epoch, port/key retirement, detach/rearm, actual fresh RSN handshake and authorization, fixed-address resume and new TCP; initial firmware/association/security and app metadata modeled\n");platform_exit(0);
        }
        vTaskDelay(1);
    }
    assert(0);
}
static void wpa_client(void *unused)
{
    (void)unused;while(!atomic_load(&wpa_result))vTaskDelay(1);
    struct sunstone_net_request r={.operation=SUNSTONE_NET_OPEN_TCP,.port=7400,.timeout=5000};
    struct sunstone_net_reply reply=request(&r);assert(reply.result==SUNSTONE_NET_OK&&reply.handle);uint64_t handle=reply.handle;
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CONNECT,.handle=handle,.port=9000,.timeout=5000};memcpy(r.address,peer_ip,4);assert(request(&r).result==SUNSTONE_NET_OK);
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_SEND,.handle=handle,.bytes=1,.timeout=5000};r.payload[0]='w';assert(request(&r).result==SUNSTONE_NET_OK);
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_RECEIVE,.handle=handle,.bytes=1,.timeout=5000};reply=request(&r);assert(reply.result==SUNSTONE_NET_OK&&reply.bytes==1&&reply.payload[0]=='w');
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CLOSE,.handle=handle,.timeout=5000};assert(request(&r).result==SUNSTONE_NET_OK);atomic_store(&wpa_result,2);vTaskSuspend(NULL);
}
void kernel_main(void)
{
    platform_init();session_task=xTaskCreateStatic(wpa_owner,"wpa-owner",8192,NULL,4,session_stack,&session_task_storage);assert(session_task);
    client_task=xTaskCreateStatic(wpa_client,"wpa-client",4096,NULL,5,client_stack,&client_task_storage);assert(client_task);
    wpa_profile();wpa_model_setup();vTaskStartScheduler();assert(0);
}
