/* Reuse host callbacks at an explicit initialized, associated checkpoint. */
#define AIC8800_RUNTIME_FIXTURE_MAIN data_runtime_unused_main
#include "aic8800-runtime.c"
#undef AIC8800_RUNTIME_FIXTURE_MAIN
#include "aic8800_data.h"
static struct aic8800_wifi wifi;
static struct aic8800_data data;
static uint8_t expected[2][1536], rx_packet[128], ethernet[1514];
static unsigned sent_data;
static int uncertain_write, late_bus;
static int data_clock(void *ctx,uint64_t *out)
{
    if(late_bus&&data.state==AIC8800_DATA_DONE)m.now=data.deadline;
    return now_cb(ctx,out);
}
static int data_write(void *ctx,uint32_t off,uint32_t value)
{
    if(uncertain_write&&off==0x200&&m.write&&m.position==4){++m.accesses;return 0;}
    int ok=write_cb(ctx,off,value);
    if(off==0x200&&m.write&&m.position==m.bytes) {
        ++sent_data;assert(m.bytes==data.request_bytes&&!memcmp(m.transmitted,data.request,m.bytes));
        assert(!memcmp(m.transmitted,expected[data.result.eapol?0:1],m.bytes));
    }
    return ok;
}
static void checkpoint(int secure)
{
    reset_model();host.state=SUNXI_SDIO_READY;host.card_hz=375000;
    host.config.ops.read32=transport_read;host.config.ops.write32=data_write;host.config.ops.now=data_clock;
    card.host=&host;card.initialized=1;card.state=SDIO_CARD_READY;card.function_count=2;card.capabilities=2;
    card.functions[1].has_manufacturer=1;card.functions[1].manufacturer=0xc8a1;card.functions[1].device=0x0182;
    card.functions[1].max_block_bytes=512;card.functions[1].enable_timeout_ms=100;
    m.card[1][3]=3;memset(&transport,0,sizeof(transport));memset(receive,0,sizeof(receive));
    memset(&firmware,0,sizeof(firmware));memset(&runtime,0,sizeof(runtime));memset(&wifi,0,sizeof(wifi));memset(&data,0,sizeof(data));
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
    wifi.state=AIC8800_WIFI_READY;wifi.vif=1;wifi.max_stations=8;wifi.max_vifs=4;wifi.ap_index=3;
    wifi.link=secure?AIC8800_WIFI_SECURITY_PENDING:AIC8800_WIFI_ASSOCIATED_OPEN;wifi.association_generation=1;
    const uint8_t mac[]={2,1,2,3,4,5},ap[]={2,0x11,0x22,0x33,0x44,0x55};
    memcpy(wifi.mac,mac,6);memcpy(wifi.target.bssid,ap,6);
    wifi.target.security=secure?AIC8800_WIFI_WPA2_PSK_CCMP:AIC8800_WIFI_OPEN;
    assert(aic8800_data_init(&data));assert(aic8800_transport_start_data(&transport,&data));
    assert(!aic8800_data_init(&data));sent_data=0;uncertain_write=late_bus=0;
}
static void frame(int control)
{
    memset(ethernet,0,sizeof(ethernet));memcpy(ethernet,wifi.target.bssid,6);memcpy(ethernet+6,wifi.mac,6);
    ethernet[12]=control?0x88:8;ethernet[13]=control?0x8e:0;
    if(control){ethernet[14]=2;ethernet[15]=1;}else for(unsigned i=14;i<sizeof(ethernet);++i)ethernet[i]=(uint8_t)i;
}
static void bus_done(void)
{
    for(unsigned i=0;i<1000&&!data.result.bus_transferred&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(data.result.bus_transferred);
}
static void rx(const uint8_t *p,unsigned n)
{
    idle();memset(receive,0,sizeof(receive));memcpy(receive,p,n);m.card[1][4]=1;
}
static void cfm(uint8_t *p,uint32_t status,uint32_t index)
{memset(p,0,12);p[0]=8;p[2]=0x12;put32(p+4,status);put32(p+8,index);}
static void raw(void)
{
    until_held();struct aic8800_data_frame f;assert(!aic8800_transport_peek_data(&transport,&f));
    unsigned commands=m.command_count;step();assert(m.command_count==commands);
    assert(aic8800_transport_consume(&transport));idle();
}
int main(int argc,char **argv)
{
    assert(argc==3||(argc==4&&(!strcmp(argv[3],"--retained-disconnect")||!strcmp(argv[3],"--high-bit-cfm"))));FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(expected,1536,2,f)==2&&fgetc(f)==EOF);assert(!fclose(f));
    f=fopen(argv[2],"rb");assert(f);assert(fread(rx_packet,1,128,f)==128&&fgetc(f)==EOF);assert(!fclose(f));
    if(argc==4&&!strcmp(argv[3],"--high-bit-cfm")) {
        checkpoint(1);frame(1);assert(aic8800_transport_send_data(&transport,ethernet,18,m.now+2000));bus_done();
        uint8_t record[12];cfm(record,6,0);record[2]=0x92;rx(record,sizeof(record));
        for(unsigned i=0;i<400&&data.state!=AIC8800_DATA_DONE&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
        struct aic8800_data_result result;assert(aic8800_transport_take_data_result(&transport,&result));
        assert(result.raw_status==6&&result.firmware_status_valid&&result.retry_required&&result.software_retry_required);
        assert(!result.acknowledged&&!result.tx_done&&result.bus_transferred);
        puts("AIC data: confirmation type bit7 is ignored; retry status remains distinct from ACK and bus completion");return 0;
    }
    if(argc==4) {
        checkpoint(1);frame(1);assert(aic8800_transport_send_data(&transport,ethernet,18,m.now+2000));bus_done();
        uint8_t record[24]={0},loss[]={3,0,1,0,0,0};message(record,0x1805,loss,6);rx(record,sizeof(record));
        for(unsigned i=0;i<400&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
        struct aic8800_sdio_response r;struct aic8800_wifi_station_event e;
        assert(aic8800_transport_peek(&transport,&r)&&r.message_id==0x1805);
        assert(aic8800_transport_peek_station(&transport,&e)&&e.kind==AIC8800_WIFI_DISCONNECTION);
        assert(data.error==AIC8800_DATA_LINK_ERROR&&data.result.bus_transferred);retained_fault();
        puts("AIC data: unsolicited disconnect retains the raw and parsed event during uncertain EAPOL TX");return 0;
    }
    checkpoint(1);frame(0);assert(!aic8800_transport_authorize_open_data(&transport,5));
    assert(!aic8800_transport_send_data(&transport,ethernet,sizeof(ethernet),m.now+2000));
    frame(1);assert(aic8800_transport_send_data(&transport,ethernet,18,m.now+2000));
    assert(!aic8800_transport_send_data(&transport,ethernet,18,m.now+2000));
    assert(!aic8800_transport_disconnect(&transport,3,m.now+2000));
    m.card[1][3]=2;for(unsigned i=0;i<100;++i)step();assert(!sent_data&&data.state==AIC8800_DATA_REQUEST);
    m.card[1][3]=3;bus_done();assert(data.state==AIC8800_DATA_WAITING&&sent_data==1);
    uint8_t reply[24];cfm(reply,0,0);rx(reply,12);raw();assert(data.state==AIC8800_DATA_WAITING);
    cfm(reply,9,17);rx(reply,12);raw();assert(data.state==AIC8800_DATA_WAITING);
    cfm(reply,9,0x80000000u);rx(reply,12);
    for(unsigned i=0;i<300&&data.state!=AIC8800_DATA_DONE;++i)step();
    struct aic8800_data_result result;assert(!aic8800_transport_take_data_result(&transport,(void *)data.request));
    assert(aic8800_transport_take_data_result(&transport,&result));
    assert(result.eapol&&result.bus_transferred&&result.firmware_status_valid&&result.tx_done&&result.acknowledged);
    assert(!result.retry_required&&!result.software_retry_required&&result.raw_index==0x80000000u);
    rx(rx_packet,128);until_held();struct aic8800_data_frame view;
    assert(aic8800_transport_peek_data(&transport,&view)&&view.eapol&&view.bytes==18&&view.generation==1);
    assert(!memcmp(view.ethernet,wifi.mac,6)&&!memcmp(view.ethernet+6,wifi.target.bssid,6));
    assert(view.ethernet[12]==0x88&&view.ethernet[14]==2);
    unsigned commands=m.command_count;step();assert(m.command_count==commands);
    assert(!aic8800_transport_peek_data(&transport,(void *)data.ethernet));
    assert(aic8800_transport_consume(&transport));idle();assert(!data.held);
    /* Unsupported encrypted/reordered/AMSDU/foreign/truncated/LLC records stay raw. */
    const unsigned offsets[]={36,48,48,49,60,84,95};
    const uint8_t masks[]={12,32,1,3,4,1,0xff};
    for(unsigned i=0;i<7;++i){uint8_t record[128];memcpy(record,rx_packet,128);record[offsets[i]]^=masks[i];rx(record,128);raw();}
    /* QoS without reorder/AMSDU has a bounded two-byte header extension. */
    uint8_t qos[128];memcpy(qos,rx_packet,128);memmove(qos+86,qos+84,12);qos[84]=qos[85]=0;qos[60]|=0x80;qos[0]+=2;
    rx(qos,128);until_held();assert(aic8800_transport_peek_data(&transport,&view)&&view.bytes==18);
    assert(aic8800_transport_consume(&transport));idle();
    cfm(reply,9,0);rx(reply,12);raw(); /* stale CFM cannot complete another packet */

    checkpoint(0);frame(0);assert(!aic8800_transport_send_data(&transport,ethernet,sizeof(ethernet),m.now+2000));
    assert(aic8800_transport_authorize_open_data(&transport,5));
    assert(!aic8800_transport_send_data(&transport,data.request,18,m.now+2000));
    assert(aic8800_transport_send_data(&transport,ethernet,sizeof(ethernet),m.now+2000));
    bus_done();assert(aic8800_transport_take_data_result(&transport,&result));
    assert(!result.eapol&&result.bus_transferred&&!result.firmware_status_valid&&!result.acknowledged);
    idle();++wifi.association_generation;
    assert(!aic8800_transport_send_data(&transport,ethernet,sizeof(ethernet),m.now+2000)&&!data.open_authority);
    assert(aic8800_transport_authorize_open_data(&transport,6));
    uint8_t ip[128];memcpy(ip,rx_packet,128);ip[90]=8;ip[91]=0;
    rx(ip,128);until_held();assert(aic8800_transport_peek_data(&transport,&view)&&!view.eapol&&view.generation==2);
    assert(aic8800_transport_consume(&transport));idle();
    wifi.link=AIC8800_WIFI_LINK_UNRESOLVED;step();assert(!data.open_authority&&data.state==AIC8800_DATA_IDLE);

    checkpoint(1);frame(1);assert(aic8800_transport_send_data(&transport,ethernet,18,m.now+2000));
    bus_done();wifi.link=AIC8800_WIFI_LINK_DOWN;step();assert(data.state==AIC8800_DATA_FAULT&&data.result.bus_transferred);retained_fault();
    checkpoint(1);frame(1);assert(aic8800_transport_send_data(&transport,ethernet,18,m.now+2000));
    uncertain_write=1;for(unsigned i=0;i<500&&transport.state!=AIC8800_TRANSPORT_FAULT;++i)step();
    assert(data.state==AIC8800_DATA_FAULT&&!data.result.bus_transferred&&data.request[2]==1);retained_fault();
    checkpoint(1);frame(1);assert(aic8800_transport_send_data(&transport,ethernet,18,m.now+2000));
    bus_done();m.now=data.deadline;step();assert(data.error==AIC8800_DATA_TIME_ERROR);retained_fault();
    checkpoint(0);frame(0);assert(aic8800_transport_authorize_open_data(&transport,5));
    assert(aic8800_transport_send_data(&transport,ethernet,sizeof(ethernet),m.now+2000));late_bus=1;
    bus_done();assert(data.state==AIC8800_DATA_FAULT&&data.result.bus_transferred);retained_fault();
    puts("AIC data: source descriptors, credits, EAPOL status, open-port authority, retained RX, generation revocation and uncertain completion passed");return 0;
}
