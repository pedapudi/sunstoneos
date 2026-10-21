/* One native session/socket composition. Existing firmware and association
 * proofs supply explicit lower-worker checkpoints; no prior suite is run. */
#include "a733_radio_session.h"
#include "platform.h"
#define main session_sdio_unused_main
#include "sdio-controller.c"
#undef main
#define main session_platform_unused_main
#include "a733-radio-platform.c"
#undef main
#ifdef SUNSTONE_RADIO_SESSION_OBJECT
#define session SUNSTONE_RADIO_SESSION_OBJECT
#else
static struct a733_radio_session session;
#endif
static TaskHandle_t session_task,client_task;
static StaticTask_t session_task_storage,client_task_storage;
static StackType_t session_stack[8192],client_stack[4096];
static atomic_uint network_started,inject_fault;
static unsigned open_authorized;
static uint64_t clock_bias,renewed_from;
static unsigned firmware_checkpoint,association_checkpoint,renewed,io_at_fault,contended,contention_done;
extern const uint8_t session_blob0[],session_blob1[],session_blob2[],session_blob3[];
static uint8_t receive[4096];
static unsigned transmissions, receive_commands, credit_reads, enable_reads, pad_writes;
static int add_unrelated, malformed, response_status, ready_delay, zero_credit, fail_setup;
static uint64_t pad_time, byte_mode_time;
static int drop_setup_confirmation, fail_pad_restore, setup_write_applied, fail_tx;
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static void confirmation(uint8_t *p, uint32_t address, uint32_t value)
{
    p[0] = 20; p[2] = 0x11; p[4] = 1; p[5] = 4; p[10] = 8;
    put32(p + 16, address); put32(p + 20, value);
}
static void tx_done(void)
{
    ++transmissions;
    int write_setup = transmissions == 1;
    assert(m.transmitted[0] == (write_setup ? 20 : 16) && m.transmitted[2] == 0x11);
    assert(m.transmitted[3] == (write_setup ? 0x7e : 0xd5));
    assert(m.transmitted[8] == (write_setup ? 2 : 0) && m.transmitted[9] == 4);
    assert(m.transmitted[14] == (write_setup ? 8 : 4));
    for (unsigned i = write_setup ? 24 : 20; i < 512; ++i) assert(m.transmitted[i] == 0);
    uint32_t address = get32(m.transmitted + 16);
    if (write_setup) {
        assert(address == 0x40500058 && get32(m.transmitted + 20) == 0x40);
        setup_write_applied = 1;
    } else {
        assert(address == (transmissions == 2 ? 0x40500000u : 4u));
        assert(session.transport.setup_confirmed && setup_write_applied);
        if (session.transport.config.pad == AIC8800_PAD_WRITE_80)
            assert(m.card[0][0xf1] == 0 && m.now - pad_time >= 10);
    }
    memset(receive, 0, sizeof(receive));
    unsigned at = 0;
    if (add_unrelated && transmissions == 1) {
        confirmation(receive, 0x12345678, 0xabcdef12); at = 24;
    }
    if (write_setup) {
        /* The published sender imposes no echo semantics on these CFM fields. */
        confirmation(receive + at, 0xdeadbeef, 0x11223344); receive[at + 4] = 3;
    } else confirmation(receive + at, address, transmissions == 2 ? 0x00c30000u : 0x70u);
    if (malformed) receive[10] = 9; /* Both confirmations declare eight bytes. */
    m.card[1][4] = drop_setup_confirmation && write_setup ? 0 :
        response_status ? (uint8_t)response_status : 1;
    m.card[1][5] = 128;
}
static int transport_read(void *context, uint32_t offset, uint32_t *out)
{
    if (offset != 0x200) return read_cb(context, offset, out);
    ++m.accesses;
    if (m.fail_fifo && m.position == 4) return 0;
    assert(m.active && !m.write && !m.stall_fifo && m.position + 4 <= m.bytes);
    *out = get32(receive + m.position); m.position += 4; ++m.words;
    if (m.position == m.bytes) { m.regs[0x38 / 4] |= 8; m.card[1][4] = 0; }
    return 1;
}
static int transport_write(void *context, uint32_t offset, uint32_t value)
{
    if (offset == 0x18 && !(value & (1u << 21))) {
        uint32_t arg = m.regs[0x1c / 4]; unsigned cmd = value & 63;
        unsigned fn = (arg >> 28) & 7, addr = (arg >> 9) & 0x1ffff;
        if (cmd == 52 && (arg >> 31)) {
            if (fn == 0 && addr == 2 && !ready_delay) m.card[0][3] |= (uint8_t)arg;
            if (fn == 0 && (addr == 0xf1 || addr == 0xf2)) { ++pad_writes; pad_time = m.now; }
            if (fail_pad_restore && fn == 0 && addr == 0xf1 && (uint8_t)arg == 0) {
                assert(write_cb(context, offset, value)); return 0;
            }
            if (fn == 1 && addr == 7) byte_mode_time = m.now;
            if (fail_setup && fn == 0 && addr == 0x111) {
                assert(write_cb(context, offset, value)); return 0; /* Write took effect. */
            }
        } else if (cmd == 52) {
            if (fn == 0 && addr == 3 && ++enable_reads >= 3) m.card[0][3] |= 2;
            if (fn == 1 && addr == 3) { ++credit_reads; m.card[1][3] = zero_credit || credit_reads < 3 ? 0 : 1; }
        } else if (cmd == 53) {
            assert(fn == 1 && !(arg & (1u << 26))); /* Fixed FIFO address. */
            assert(m.card[0][0x110] == 0 && m.card[0][0x111] == 2 && (m.card[0][3] & 2));
            if (arg >> 31) assert(addr == 0x10 && m.regs[0x14 / 4] == 512);
            else { assert(addr == 0x0f); ++receive_commands; }
        }
    }
    if (fail_tx && offset == 0x200 && m.write && m.position == 4) { ++m.accesses; return 0; }
    int result = write_cb(context, offset, value);
    if (offset == 0x200 && m.write && m.position == m.bytes) tx_done();
    return result;
}
static uint8_t local_mac[]={2,1,2,3,4,5},peer_mac[]={2,17,34,51,68,85};
static uint8_t local_ip[]={10,0,0,2},peer_ip[]={10,0,0,1};
static struct {uint8_t bytes[1514];unsigned length;} peer_rx[8];
static unsigned peer_head,peer_count,udp_packets,tcp_packets,syn_packets;
static uint32_t tcp_next,peer_next=9001;
static uint16_t be16(const uint8_t *p){return (uint16_t)((unsigned)p[0]<<8|p[1]);}
static uint32_t be32(const uint8_t *p){return (uint32_t)be16(p)<<16|be16(p+2);}
static void store16(uint8_t *p,uint16_t x){p[0]=(uint8_t)(x>>8);p[1]=(uint8_t)x;}
static void store32(uint8_t *p,uint32_t x){store16(p,(uint16_t)(x>>16));store16(p+2,(uint16_t)x);}
static uint32_t sum(const uint8_t *p,unsigned bytes,uint32_t total)
{for(unsigned i=0;i+1<bytes;i+=2)total+=be16(p+i);if(bytes&1)total+=(uint32_t)p[bytes-1]<<8;return total;}
static uint16_t checksum(uint32_t total){while(total>>16)total=(total&65535)+(total>>16);return (uint16_t)~total;}
static uint16_t protocol_checksum(const uint8_t *ip,const uint8_t *p,unsigned bytes,unsigned protocol)
{return checksum(sum(p,bytes,sum(ip+12,8,bytes+protocol)));}
static void enqueue(const uint8_t *p,unsigned n)
{assert(peer_count<8&&n<=1514);unsigned at=(peer_head+peer_count)%8;memcpy(peer_rx[at].bytes,p,n);peer_rx[at].length=n;++peer_count;}
static void ip_header(uint8_t *p,unsigned payload,unsigned proto)
{
    memcpy(p,local_mac,6);memcpy(p+6,peer_mac,6);p[12]=8;p[13]=0;
    uint8_t *ip=p+14;ip[0]=0x45;store16(ip+2,(uint16_t)(20+payload));ip[8]=64;ip[9]=(uint8_t)proto;
    memcpy(ip+12,peer_ip,4);memcpy(ip+16,local_ip,4);store16(ip+10,checksum(sum(ip,20,0)));
}
static void peer(const uint8_t *p,unsigned n)
{
    assert(n>=14);assert(!memcmp(p+6,local_mac,6));
    if(be16(p+12)==0x806){
        assert(n>=42&&be16(p+14)==1&&be16(p+16)==0x800&&p[18]==6&&p[19]==4);
        if(be16(p+20)!=1||memcmp(p+38,peer_ip,4))return;
        uint8_t reply[42]={0};memcpy(reply,local_mac,6);memcpy(reply+6,peer_mac,6);store16(reply+12,0x806);
        memcpy(reply+14,p+14,6);store16(reply+20,2);memcpy(reply+22,peer_mac,6);memcpy(reply+28,peer_ip,4);
        memcpy(reply+32,local_mac,6);memcpy(reply+38,local_ip,4);enqueue(reply,sizeof(reply));return;
    }
    assert(be16(p+12)==0x800&&n>=34);const uint8_t *ip=p+14;unsigned ihl=(ip[0]&15)*4,bytes=be16(ip+2);
    assert(ihl>=20&&ihl<=bytes&&bytes<=n-14&&!checksum(sum(ip,ihl,0)));
    if(memcmp(ip+16,peer_ip,4))return;
    const uint8_t *payload=ip+ihl;unsigned length=bytes-ihl;
    if(ip[9]==17){
        assert(length>=8&&be16(payload+4)==length&&!protocol_checksum(ip,payload,length,17));++udp_packets;
        uint8_t reply[1514]={0};ip_header(reply,length,17);memcpy(reply+34,payload,length);
        store16(reply+34,be16(payload+2));store16(reply+36,be16(payload));store16(reply+40,0);
        store16(reply+40,protocol_checksum(reply+14,reply+34,length,17));enqueue(reply,34+length);return;
    }
    assert(ip[9]==6&&length>=20&&!protocol_checksum(ip,payload,length,6));
    unsigned header=(payload[12]>>4)*4;assert(header>=20&&header<=length);
    uint8_t flags=payload[13];unsigned data_bytes=length-header;
    if(!(flags&2)&&!data_bytes)return;
    uint8_t reply[1514]={0};ip_header(reply,20+data_bytes,6);uint8_t *tcp=reply+34;
    store16(tcp,be16(payload+2));store16(tcp+2,be16(payload));tcp[12]=5<<4;store16(tcp+14,8192);
    if(flags&2){++syn_packets;tcp_next=be32(payload+4)+1;store32(tcp+4,9000);store32(tcp+8,tcp_next);tcp[13]=0x12;}
    else{++tcp_packets;assert(be32(payload+4)==tcp_next);tcp_next+=data_bytes;store32(tcp+4,peer_next);store32(tcp+8,tcp_next);
        tcp[13]=0x18;memcpy(tcp+20,payload+header,data_bytes);peer_next+=data_bytes;}
    store16(tcp+16,protocol_checksum(reply+14,tcp,20+data_bytes,6));enqueue(reply,54+data_bytes);
}
static int network_write(void *context,uint32_t offset,uint32_t value)
{
    int ok=write_cb(context,offset,value);
    if(offset==0x200&&m.write&&m.position==m.bytes){
        assert(m.transmitted[2]==1);unsigned payload=m.transmitted[4]|(unsigned)m.transmitted[5]<<8;
        assert(payload<=1500&&payload+32<=m.bytes);uint8_t ethernet[1514];
        memcpy(ethernet,m.transmitted+12,14);memcpy(ethernet+14,m.transmitted+32,payload);peer(ethernet,payload+14);
    }
    return ok;
}

static int model_entropy(void *context,uint32_t *value)
{assert(context==&m);*value=0x13579bdf;return 1;} /* Model only; no entropy qualification. */
static int model_sequence(void *context,uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,uint32_t *value)
{(void)src;(void)sport;(void)dst;(void)dport;return model_entropy(context,value);}
static int model_id(void *context,uint8_t id[16])
{assert(context==&m);for(unsigned i=0;i<16;++i)id[i]=(uint8_t)(i+32);return 1;}
static void open_rx(void)
{
    struct aic8800_sdio_transport *t=&session.transport;
    if(!peer_count||m.card[1][4]||t->pending||t->held||t->cursor!=t->rx_bytes)return;
    const uint8_t *p=peer_rx[peer_head].bytes;unsigned n=peer_rx[peer_head].length;
    memset(receive,0,sizeof(receive));unsigned bytes=24+8+n-14;
    receive[0]=(uint8_t)bytes;receive[1]=(uint8_t)(bytes>>8);
    receive[48]=0x40;receive[49]=1;receive[50]=3;receive[51]=0xff;
    receive[60]=8;receive[61]=2;memcpy(receive+64,p,6);memcpy(receive+70,peer_mac,6);memcpy(receive+76,p+6,6);
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0};memcpy(receive+84,snap,6);memcpy(receive+90,p+12,n-12);
    m.card[1][4]=(uint8_t)((60+bytes+511)/512);peer_head=(peer_head+1)%8;--peer_count;
}
static int session_now(void *context,uint64_t *out)
{
    /* Simulate another owner's actual registry mutex at the transition between
     * accepted transport renewal and platform authentication of that bound. */
    if(renewed_from&&!contended&&session.renew_pending&&session.transport.deadline>renewed_from){
        atomic_store(&r.power.clocks.busy,1);contended=1;
    }
    return radio_now(context,out);
}
static int session_current(void *context)
{return context==&r && xTaskGetCurrentTaskHandle()==session_task;}
static int session_read(void *context,uintptr_t a,uint32_t *out)
{
    if(a>=0x04021000&&a<=0x04021300){host_accessible(context);return transport_read(&m,a-0x04021000,out);}
    return radio_read(context,a,out);
}
static int session_write(void *context,uintptr_t a,uint32_t value)
{
    if(a>=0x04021000&&a<=0x04021300){
        host_accessible(context);
        return session.transport.state==AIC8800_TRANSPORT_RUNTIME ?
            network_write(&m,a-0x04021000,value):transport_write(&m,a-0x04021000,value);
    }
    return radio_write(context,a,value);
}
static void model_setup(void)
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
    struct a733_radio_platform_ops ops={session_now,session_current,radio_admit,radio_enter,radio_leave,radio_retain,session_read,session_write,radio_barrier};
    assert(a733_radio_platform_init(&r.radio,&p,&ops,&r,&r.power.clocks,&r.power.pins,&r.power.rails,&r.power.provider));
    struct a733_radio_session_config c={.owner_task=session_task,.owner=80,.bootstrap_deadline=60000000,
        .runtime_lease_us=10000000,.renew_margin_us=3000000,
        .transport={.variant=AIC8800_TRANSPORT_D80_BOOTSTRAP,.pad=AIC8800_PAD_KEEP,.ticks_per_ms=1000,.command_timeout=100000,.poll_interval=1000},
        .bootstrap={1,0x40500058,0x40,1,1},
        .firmware={.identity={.chip_word=0x00c30000,.subrevision_word=0x70},.profile=1,
            .blobs={{session_blob0,1708},{session_blob1,24744},{session_blob2,23288},{session_blob3,341260}}},
        .runtime={model_id,&m,1000000},
        .wifi={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=1000000,.profile=1,.channel_count=1,.channels={{2412,0,1,17}}}};
    const uint8_t *blob=c.firmware.blobs[0].data;
    c.firmware.blobs[0].data=(const uint8_t *)&r.power.now;
    assert(!a733_radio_session_init(&session,&r.radio,&c)&&!session.self);
    c.firmware.blobs[0].data=blob;
    assert(a733_radio_session_init(&session,&r.radio,&c));
}
static void adopted_checkpoints(void)
{
    struct aic8800_sdio_transport *t=&session.transport;
    /* Reuse the authenticated upload transcript only after actual card,
     * identity and firmware-owner admission. No session state is fabricated. */
    if(!firmware_checkpoint&&t->firmware==&session.firmware&&session.firmware.hash_offset>=64){
        assert(t->state==AIC8800_TRANSPORT_FIRMWARE&&session.firmware.owner==t);
        assert(t->card==&r.radio.card&&session.borrow.generation==r.radio.clock.id);
        assert(a733_clock_token_status(&r.power.clocks,&r.radio.clock)==A733_CLOCK_OK);
        assert(!t->pending&&r.radio.host.state==SUNXI_SDIO_READY);
        session.firmware.state=AIC8800_FW_START_CONFIRMED;t->state=AIC8800_TRANSPORT_START_CONFIRMED;
        firmware_checkpoint=1;
    }
    /* Existing management/association evidence supplies only lower-worker
     * state, after actual runtime, dispatcher and Wi-Fi owner attachment. */
    if(!association_checkpoint&&t->wifi==&session.wifi){
        struct aic8800_wifi *w=&session.wifi;
        assert(firmware_checkpoint&&w->owner==t&&t->receive_owner==&session.dispatcher);
        assert(session.dispatcher.initialized&&session.dispatcher.transport==t);
        w->state=AIC8800_WIFI_READY;w->vif=1;w->max_stations=8;w->max_vifs=4;w->ap_index=3;
        w->link=AIC8800_WIFI_ASSOCIATED_OPEN;w->association_generation=1;
        memcpy(w->mac,local_mac,6);memcpy(w->target.bssid,peer_mac,6);w->target.security=AIC8800_WIFI_OPEN;
        association_checkpoint=1;m.card[1][3]=3;
    }
}
static void session_worker(void *unused)
{
    (void)unused;uint64_t borrowed=0;
    for(unsigned iteration=0;iteration<20000;++iteration){
        r.power.now=100+(uint64_t)xTaskGetTickCount()*1000+clock_bias;m.now=r.power.now;
        if(!session.fault)assert(pmic_model_step(&r.power)!=A733_PMIC_RETAINED);
        if(atomic_load(&network_started)||open_authorized)open_rx();
        if(atomic_load(&inject_fault)&&session.sockets.network.active&&session.transport.pending&&!session.fault){
            assert(session.sockets.network.owns_current);
            io_at_fault=m.accesses+r.accesses;
            assert(a733_clock_retain(&r.power.clocks,&r.radio.clock)==A733_CLOCK_FAULT);
        }
        unsigned had_pins=r.radio.pin_lease.id!=0,enabled=*pmic_model_word(&r.power,0x07025040)&2;
        enum a733_radio_session_result status=a733_radio_session_poll(&session);
        if(!had_pins&&r.radio.pin_lease.id)r.pins_at=r.power.now;
        if(!enabled&&(*pmic_model_word(&r.power,0x07025040)&2))r.enabled_at=r.power.now;
        if(contended&&!contention_done){
            assert(status==A733_SESSION_BUSY&&session.renew_pending);
            unsigned accesses=m.accesses+r.accesses;size_t cursor=session.transport.cursor;
            uint64_t retired=session.dispatcher.retired_records;
            assert(a733_radio_session_poll(&session)==A733_SESSION_BUSY&&session.renew_pending);
            assert(accesses==m.accesses+r.accesses&&cursor==session.transport.cursor&&
                retired==session.dispatcher.retired_records&&!session.sockets.initialized);
            atomic_store(&r.power.clocks.busy,0);contention_done=1;
        }
        if(status==A733_SESSION_RETAINED){
            assert(atomic_load(&inject_fault)&&m.accesses+r.accesses==io_at_fault);
            assert(session.sockets.network.owns_current&&session.sockets.network.active);
        }else assert(status!=A733_SESSION_INVALID);
        if(session.borrow.generation){if(!borrowed)borrowed=session.borrow.generation;assert(session.borrow.generation==borrowed);}
        adopted_checkpoints();
        if(status==A733_SESSION_MANAGEMENT&&!renewed_from){
            renewed_from=session.deadline;
            clock_bias+=session.deadline-r.power.now-session.config.renew_margin_us/2;
        }
        if(renewed_from&&session.deadline>renewed_from&&!session.renew_pending){
            assert(session.deadline==r.radio.deadline&&session.deadline==session.transport.deadline);renewed=1;
        }
        if(status==A733_SESSION_MANAGEMENT&&renewed&&!atomic_load(&network_started)){
            if(!open_authorized&&a733_radio_session_authorize_open(&session,91)){
                /* A broadcast received after port authorization but before
                 * socket attachment must be discarded, not held forever. */
                uint8_t arp[42]={0xff,0xff,0xff,0xff,0xff,0xff};memcpy(arp+6,peer_mac,6);
                store16(arp+12,0x806);store16(arp+14,1);store16(arp+16,0x800);arp[18]=6;arp[19]=4;store16(arp+20,1);
                memcpy(arp+22,peer_mac,6);memcpy(arp+28,peer_ip,4);memcpy(arp+38,(uint8_t[]){10,0,0,99},4);
                enqueue(arp,sizeof(arp));open_authorized=1;
            }
            if(open_authorized&&session.discarded_data==1&&!peer_count&&
               aic8800_transport_attachment_ready(&session.transport)){
                struct sunstone_net_config c={.network={.transport=&session.transport,.dispatcher=&session.dispatcher,
                    .address={10,0,0,2},.netmask={255,255,255,0},.gateway={10,0,0,1},.tx_timeout=2000000,.queue_timeout=3000,
                    .random32=model_entropy,.sequence=model_sequence,.entropy_context=&m,.entropy_record=1},
                    .bootstrap=session_task,.clients={client_task}};
                assert(a733_radio_session_start_network(&session,&c));
                assert(session.sockets.worker==session_task&&session.sockets.owned_poll);
                atomic_store(&network_started,1);
            }
        }
        vTaskDelay(1);
    }
    assert(0);
}
static struct sunstone_net_reply request(struct sunstone_net_request *r)
{
    uint64_t ticket;assert(sunstone_net_submit(&session.sockets,r,&ticket));
    struct sunstone_net_reply reply;assert(sunstone_net_receive(&session.sockets,&reply,r->timeout+100));
    assert(reply.ticket==ticket);return reply;
}
static void client_worker(void *unused)
{
    (void)unused;while(!atomic_load(&network_started))vTaskDelay(1);
    assert(a733_radio_session_poll(&session)==A733_SESSION_INVALID);
    assert(!sunstone_net_poll_owned(&session.sockets));
    struct sunstone_net_request r={.operation=SUNSTONE_NET_OPEN_TCP,.port=7400,.timeout=5000};
    struct sunstone_net_reply reply=request(&r);assert(reply.result==SUNSTONE_NET_OK&&reply.handle);uint64_t socket=reply.handle;
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CONNECT,.handle=socket,.port=9000,.timeout=5000};memcpy(r.address,peer_ip,4);
    assert(request(&r).result==SUNSTONE_NET_OK);
    const char payload[]="owned radio TCP";
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_SEND,.handle=socket,.timeout=5000,.bytes=sizeof(payload)-1};memcpy(r.payload,payload,r.bytes);
    assert(request(&r).result==SUNSTONE_NET_OK);
    r.operation=SUNSTONE_NET_RECEIVE;r.bytes=1472;reply=request(&r);
    assert(reply.result==SUNSTONE_NET_OK&&reply.bytes==sizeof(payload)-1&&!memcmp(reply.payload,payload,reply.bytes));
    assert(syn_packets==1&&tcp_packets==1&&renewed&&contention_done&&firmware_checkpoint&&association_checkpoint&&
        session.discarded_data==1);
    atomic_store(&inject_fault,1);
    r.operation=SUNSTONE_NET_SEND;r.bytes=sizeof(payload)-1;memcpy(r.payload,payload,r.bytes);assert(request(&r).result==SUNSTONE_NET_OK);
    r.operation=SUNSTONE_NET_RECEIVE;r.bytes=1472;reply=request(&r);assert(reply.result==SUNSTONE_NET_LINK_LOST);
    assert(session.fault&&!aic8800_network_ready(&session.sockets.network));
    assert(session.sockets.sockets[0].socket&&session.borrow.generation==session.platform->clock.id);
    platform_puts("PASS actual radio session ownership, staged lease renewal, pre-network broadcast discard, native TCP queues and retained live-guard failure; firmware/association checkpoints and entropy are modeled\n");
    platform_exit(0);
}
/* Keep the existing wrapper link options without altering stack behavior. */
int32_t __real_FreeRTOS_sendto(Socket_t,const void *,size_t,BaseType_t,const struct freertos_sockaddr *,socklen_t);
int32_t __wrap_FreeRTOS_sendto(Socket_t socket,const void *p,size_t n,BaseType_t flags,const struct freertos_sockaddr *peer,socklen_t bytes)
{return __real_FreeRTOS_sendto(socket,p,n,flags,peer,bytes);}
Socket_t __real_FreeRTOS_accept(Socket_t,struct freertos_sockaddr *,socklen_t *);
Socket_t __wrap_FreeRTOS_accept(Socket_t socket,struct freertos_sockaddr *peer,socklen_t *bytes)
{return __real_FreeRTOS_accept(socket,peer,bytes);}
struct xSOCKET *__real_prvHandleListen_IPV4(struct xSOCKET *,struct xNETWORK_BUFFER *);
struct xSOCKET *__wrap_prvHandleListen_IPV4(struct xSOCKET *parent,struct xNETWORK_BUFFER *packet)
{return __real_prvHandleListen_IPV4(parent,packet);}
_Noreturn void __assert_func(const char *file,int line,const char *function,const char *expression)
{(void)function;platform_puts(expression);platform_puts("\n");platform_panic(file,(unsigned)line);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void kernel_main(void)
{
    platform_init();
    session_task=xTaskCreateStatic(session_worker,"radio-session",8192,NULL,4,session_stack,&session_task_storage);assert(session_task);
    client_task=xTaskCreateStatic(client_worker,"socket-client",4096,NULL,5,client_stack,&client_task_storage);assert(client_task);
    model_setup();vTaskStartScheduler();assert(0);
}
