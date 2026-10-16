/* Native FreeRTOS/IP tasks with real radio data/SDIO/controller workers.
 * Firmware authentication/decryption and entropy are synthetic checkpoint
 * inputs, not board or production-entropy evidence. */
#define AIC8800_RUNTIME_FIXTURE_MAIN network_runtime_unused_main
#include "aic8800-runtime.c"
#undef AIC8800_RUNTIME_FIXTURE_MAIN
#include "sunstone_net.h"
#include "aic8800_supplicant.h"
#include "NetworkBufferManagement.h"
#include "FreeRTOS_IP_Private.h"
#include "platform.h"
static struct sunstone_net net;
static struct aic8800_wifi wifi;
static struct aic8800_data data;
static struct aic8800_security security;
static struct aic8800_supplicant supplicant;
static StaticTask_t app_task;static StackType_t app_stack[4096];
static uint8_t local_mac[]={2,1,2,3,4,5},peer_mac[]={2,17,34,51,68,85};
static uint8_t local_ip[]={10,0,0,2},peer_ip[]={10,0,0,1};
static struct {uint8_t bytes[1514];unsigned length;} peer_rx[8];
static unsigned peer_head,peer_count,udp_packets,tcp_packets,syn_packets;
static uint64_t peer_pn;
static uint32_t tcp_next,peer_next=9001;
static int entropy_failure,case_entropy,case_late,case_alias;
int32_t __real_FreeRTOS_sendto(Socket_t,const void *,size_t,BaseType_t,const struct freertos_sockaddr *,socklen_t);
int32_t __wrap_FreeRTOS_sendto(Socket_t socket,const void *p,size_t n,BaseType_t flags,const struct freertos_sockaddr *peer,socklen_t bytes)
{
    int32_t result=__real_FreeRTOS_sendto(socket,p,n,flags,peer,bytes);
    if(case_late&&result>0)vTaskDelay(20);
    return result;
}
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
static void pump_rx(void)
{
    if(!peer_count||m.card[1][4]||transport.pending||transport.held||transport.cursor!=transport.rx_bytes)return;
    const uint8_t *p=peer_rx[peer_head].bytes;unsigned n=peer_rx[peer_head].length;
    assert(n>=14);memset(receive,0,sizeof(receive));unsigned bytes=24+8+8+n-14;
    receive[0]=(uint8_t)bytes;receive[1]=(uint8_t)(bytes>>8);receive[36]=12;receive[48]=0x40;receive[49]=1;receive[50]=3;receive[51]=0xff;
    receive[60]=8;receive[61]=0x42;memcpy(receive+64,p,6);memcpy(receive+70,peer_mac,6);memcpy(receive+76,p+6,6);
    ++peer_pn;receive[84]=(uint8_t)peer_pn;receive[85]=(uint8_t)(peer_pn>>8);receive[87]=0x20;
    for(unsigned i=0;i<4;++i)receive[88+i]=(uint8_t)(peer_pn>>(16+8*i));
    const uint8_t snap[]={0xaa,0xaa,3,0,0,0};memcpy(receive+92,snap,6);memcpy(receive+98,p+12,n-12);
    m.card[1][4]=(uint8_t)((60+bytes+511)/512);peer_head=(peer_head+1)%8;--peer_count;
}
static int network_clock(void *context,uint64_t *now)
{m.now=100+xTaskGetTickCount();pump_rx();return now_cb(context,now);}
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
static int random32(void *context,uint32_t *value)
{assert(context==&m);if(entropy_failure)return 0;*value=0x13579bdf;return 1;}
static int sequence(void *context,uint32_t src,uint16_t sport,uint32_t dst,uint16_t dport,uint32_t *value)
{(void)src;(void)sport;(void)dst;(void)dport;return random32(context,value);}
static void checkpoint(void)
{
    reset_model();host.state=SUNXI_SDIO_READY;host.card_hz=375000;host.config.ops.read32=transport_read;
    host.config.ops.write32=network_write;host.config.ops.now=network_clock;
    card.host=&host;card.initialized=1;card.state=SDIO_CARD_READY;card.function_count=2;card.capabilities=2;
    card.functions[1].has_manufacturer=1;card.functions[1].manufacturer=0xc8a1;card.functions[1].device=0x0182;
    card.functions[1].max_block_bytes=512;card.functions[1].enable_timeout_ms=100;m.card[1][3]=3;
    struct aic8800_transport_config tc={.variant=AIC8800_TRANSPORT_D80_BOOTSTRAP,.pad=AIC8800_PAD_KEEP,
        .ticks_per_ms=1,.command_timeout=2000,.poll_interval=1};assert(aic8800_transport_init(&transport,&card,&tc));
    firmware.initialized=1;firmware.owner=&transport;firmware.state=AIC8800_FW_START_CONFIRMED;
    transport.firmware=&firmware;transport.state=AIC8800_TRANSPORT_START_CONFIRMED;
    struct aic8800_runtime_config rc={.read_platform_id=platform_id,.context=&m,.binding_timeout=3000};
    assert(aic8800_runtime_init(&runtime,&rc));assert(aic8800_transport_start_runtime(&transport,&runtime,m.now+60000));
    struct aic8800_wifi_config wc={.rf_profile_record=1,.channel_policy_record=2,.command_timeout=1000,.profile=1,.channel_count=1,.channels={{2412,0,1,17}}};
    assert(aic8800_wifi_init(&wifi,&wc));assert(aic8800_transport_start_wifi(&transport,&wifi,m.now+50000));
    wifi.state=AIC8800_WIFI_READY;wifi.vif=1;wifi.max_stations=8;wifi.max_vifs=4;wifi.ap_index=3;wifi.link=AIC8800_WIFI_SECURED;wifi.association_generation=1;
    memcpy(wifi.mac,local_mac,6);memcpy(wifi.target.bssid,peer_mac,6);wifi.target.security=AIC8800_WIFI_WPA2_PSK_CCMP;
    assert(aic8800_data_init(&data));assert(aic8800_transport_start_data(&transport,&data));
    security.initialized=1;security.owner=&transport;security.wifi=&wifi;security.supplicant=&supplicant;
    security.generation=1;security.last_now=m.now;security.authorized=security.port_confirmed=1;
    security.keys[0].valid=1;security.keys[0].pairwise=1;security.keys[0].generation=1;
    supplicant.initialized=1;supplicant.owner=&transport;supplicant.security=&security;supplicant.wifi=&wifi;supplicant.data=&data;
    supplicant.generation=1;supplicant.last_now=m.now;supplicant.deadline=m.now+50000;supplicant.state=AIC8800_SUPPLICANT_AUTHORIZED;
    supplicant.config.command_timeout=3000;supplicant.config.ticks_per_second=1000;
    transport.security=&security;transport.supplicant=&supplicant;data.security=&security;
}
static struct sunstone_net_reply call(struct sunstone_net_request *request)
{
    uint64_t ticket;assert(sunstone_net_submit(&net,request,&ticket));struct sunstone_net_reply reply;
    assert(sunstone_net_receive(&net,&reply,request->timeout+100));assert(reply.ticket==ticket);return reply;
}
static uint64_t open_socket(int tcp,unsigned port)
{
    struct sunstone_net_request r={.operation=tcp?SUNSTONE_NET_OPEN_TCP:SUNSTONE_NET_OPEN_UDP,.port=(uint16_t)port,.timeout=3000};
    struct sunstone_net_reply result=call(&r);if(result.result!=SUNSTONE_NET_OK){platform_puts("open result/transport/data/phase/error ");platform_hex(result.result);platform_hex(transport.state);platform_hex(data.state);platform_hex(transport.phase);platform_hex(transport.error);platform_puts("\n");}
    assert(result.result==SUNSTONE_NET_OK&&result.handle);return result.handle;
}
static void close_socket(uint64_t handle)
{struct sunstone_net_request r={.operation=SUNSTONE_NET_CLOSE,.handle=handle,.timeout=3000};assert(call(&r).result==SUNSTONE_NET_OK);}
static void exchange(uint64_t handle,const char *text)
{
    struct sunstone_net_request r={.operation=SUNSTONE_NET_SEND,.handle=handle,.port=9000,.timeout=5000,.bytes=(uint16_t)strlen(text)};
    memcpy(r.address,peer_ip,4);memcpy(r.payload,text,r.bytes);struct sunstone_net_reply result=call(&r);
    assert(result.result==SUNSTONE_NET_OK&&result.stack_result==r.bytes);
    r.operation=SUNSTONE_NET_RECEIVE;r.bytes=1472;result=call(&r);
    if(result.result!=SUNSTONE_NET_OK){platform_puts("receive result/transport/error/held/rx/tx/peer ");platform_hex(result.result);platform_hex(transport.state);platform_hex(transport.error);platform_hex(transport.held);platform_hex(net.network.rx_packets);platform_hex(net.network.tx_bus_completions);platform_hex(peer_count);platform_puts("\n");}
    assert(result.result==SUNSTONE_NET_OK&&result.bytes==strlen(text)&&!memcmp(result.payload,text,result.bytes));
}
static void application(void *context)
{
    (void)context;
    if(case_alias){
        static _Alignas(8) uint8_t immutable_blob[16];
        firmware.config.blobs[0]=(struct aic8800_firmware_blob){immutable_blob,sizeof(immutable_blob)};
        struct sunstone_net_request r={.operation=SUNSTONE_NET_OPEN_UDP,.timeout=10};
        assert(!sunstone_net_submit(&net,&r,(uint64_t *)immutable_blob));
        assert(!sunstone_net_receive(&net,(struct sunstone_net_reply *)immutable_blob,0));
        assert(!net.clients[0].pending&&!immutable_blob[0]);
        platform_puts("PASS network: application outputs cannot overwrite retained firmware blobs\n");platform_exit(0);
    }
    assert(sunstone_net_activate(&net));
    for(unsigned i=0;i<100&&!aic8800_network_ready(&net.network);++i)vTaskDelay(1);
    assert(aic8800_network_ready(&net.network));
    if(case_entropy){
        uint64_t tcp=open_socket(1,7400);entropy_failure=1;unsigned old_syn=syn_packets;
        struct sunstone_net_request r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CONNECT,.handle=tcp,.timeout=1000,.port=9000};memcpy(r.address,peer_ip,4);
        struct sunstone_net_reply retained=call(&r);assert(retained.result==SUNSTONE_NET_LINK_LOST&&syn_packets==old_syn&&!aic8800_network_ready(&net.network));
        platform_puts("PASS network: entropy failure blocks SYN output\n");
        platform_exit(0);
    }
    if(case_late){
        uint64_t udp=open_socket(0,7450);
        struct sunstone_net_request r={.operation=SUNSTONE_NET_SEND,.handle=udp,.timeout=10,.port=9000,.bytes=4};memcpy(r.address,peer_ip,4);
        struct sunstone_net_reply result=call(&r);
        assert(result.result==SUNSTONE_NET_TIMEOUT&&result.stack_result==4&&result.handle==udp);
        close_socket(udp);platform_puts("PASS network: late send retains actual accepted-byte result and handle\n");platform_exit(0);
    }
    assert(!pvPortMalloc(configTOTAL_HEAP_SIZE)); /* Definite refusal preserves the finite heap. */
    uint64_t udp=open_socket(0,7000);
    /* Pinned UDP output consumes a cache-miss datagram to generate ARP. */
    struct sunstone_net_request probe={.operation=SUNSTONE_NET_SEND,.handle=udp,.port=9000,.timeout=1000,.bytes=1};
    memcpy(probe.address,peer_ip,4);assert(call(&probe).result==SUNSTONE_NET_OK);
    for(unsigned i=0;i<1000&&!net.network.rx_packets;++i)vTaskDelay(1);
    assert(net.network.rx_packets&&udp_packets==0);vTaskDelay(2);
    exchange(udp,"native UDP");
    struct sunstone_net_request r={.operation=SUNSTONE_NET_RECEIVE,.handle=udp,.timeout=10,.bytes=32};
    assert(call(&r).result==SUNSTONE_NET_TIMEOUT);exchange(udp,"UDP after timeout");
    uint64_t tcp=open_socket(1,7001);r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CONNECT,.handle=tcp,.timeout=5000,.port=9000};
    memcpy(r.address,peer_ip,4);assert(call(&r).result==SUNSTONE_NET_OK);exchange(tcp,"native TCP");
    assert(syn_packets==1&&tcp_packets==1&&udp_packets==2&&net.network.rx_packets>=4);
    close_socket(tcp);close_socket(udp);
    /* Definite socket limit refusal and successful disposal permit reuse. */
    uint64_t handles[4];for(unsigned i=0;i<4;++i)handles[i]=open_socket(0,7100+i);
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_OPEN_TCP,.timeout=100,.port=7200};assert(call(&r).result==SUNSTONE_NET_BUSY);
    for(unsigned i=0;i<4;++i)close_socket(handles[i]);
    /* A timed-out close retains its handle after disposal. The allocator may
     * immediately reuse its address for a different owned socket. */
    uint64_t old_handle=open_socket(0,7250);struct sunstone_net_socket *old_record=NULL;
    for(unsigned i=0;i<4;++i)if(net.sockets[i].handle==old_handle)old_record=&net.sockets[i];
    assert(old_record);uintptr_t old_address=(uintptr_t)old_record->socket;
    vTaskSuspend(FreeRTOS_GetIPTaskHandle());
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_CLOSE,.handle=old_handle,.timeout=10};
    assert(call(&r).result==SUNSTONE_NET_TIMEOUT);vTaskResume(FreeRTOS_GetIPTaskHandle());
    for(unsigned i=0;i<100&&!atomic_load(&old_record->disposed);++i)vTaskDelay(1);
    assert(atomic_load(&old_record->disposed)&&!atomic_load(&old_record->disposal_address));
    uint64_t reused=open_socket(0,7251);struct sunstone_net_socket *new_record=NULL;
    for(unsigned i=0;i<4;++i)if(net.sockets[i].handle==reused)new_record=&net.sockets[i];
    assert(new_record&&(uintptr_t)new_record->socket==old_address);
    close_socket(reused);close_socket(old_handle);
    /* Output spans may not overwrite any retained radio object. */
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_OPEN_UDP,.timeout=10};
    assert(!sunstone_net_submit(&net,&r,(uint64_t *)&host));
    assert(!sunstone_net_receive(&net,(struct sunstone_net_reply *)&transport,0));
    /* Queue saturation cannot create an unbounded bind wait. The timeout reply
     * retains its allocated socket, then explicit close observes real disposal. */
    TaskHandle_t ip_task=FreeRTOS_GetIPTaskHandle();vTaskSuspend(ip_task);
    IPStackEvent_t event={eNoEvent,NULL};unsigned queued=0;while(xSendEventStructToIPTask(&event,0)==pdPASS){assert(++queued<=ipconfigEVENT_QUEUE_LENGTH);}
    r=(struct sunstone_net_request){.operation=SUNSTONE_NET_OPEN_UDP,.timeout=10,.port=7300};struct sunstone_net_reply retained=call(&r);
    assert(retained.result==SUNSTONE_NET_TIMEOUT&&retained.handle);vTaskResume(ip_task);close_socket(retained.handle);
    {
        udp=open_socket(0,7400);++wifi.association_generation;
        r=(struct sunstone_net_request){.operation=SUNSTONE_NET_SEND,.handle=udp,.timeout=1000,.port=9000,.bytes=4};memcpy(r.address,peer_ip,4);
        retained=call(&r);assert(retained.result==SUNSTONE_NET_LINK_LOST&&!aic8800_network_ready(&net.network));
        assert(net.sockets[0].socket||net.sockets[1].socket||net.sockets[2].socket||net.sockets[3].socket);
        platform_puts("PASS network: real TCP/UDP, bounded sockets/bind/disposal; generation change revokes retained handles\n");
    }
    platform_exit(0);
}
_Noreturn void __assert_func(const char *file,int line,const char *function,const char *expression)
{(void)function;(void)expression;platform_panic(file,(unsigned)line);}
void vApplicationStackOverflowHook(TaskHandle_t task,char *name){(void)task;platform_panic(name,0);}
static void arguments(void)
{
    char command[128]={0};uint64_t block[2]={(uintptr_t)command,sizeof(command)};
    register uint64_t operation __asm__("x0")=0x15;register uint64_t *arg __asm__("x1")=block;
    __asm__ volatile("hlt #0xf000":"+r"(operation):"r"(arg):"memory");
    if(operation==0)for(unsigned i=0;i+7<sizeof(command);++i){
        if(!memcmp(command+i,"entropy",7))case_entropy=1;
        if(!memcmp(command+i,"late",4))case_late=1;
        if(!memcmp(command+i,"alias",5))case_alias=1;
    }
}
void kernel_main(void)
{
    platform_init();arguments();checkpoint();
    TaskHandle_t app=xTaskCreateStatic(application,"network-app",4096,NULL,5,app_stack,&app_task);assert(app);
    struct sunstone_net_config c={.network={.transport=&transport,.tx_timeout=5000,.queue_timeout=5000,
        .random32=random32,.sequence=sequence,.entropy_context=&m,.entropy_record=1,
        .address={10,0,0,2},.netmask={255,255,255,0},.gateway={10,0,0,1}},.bootstrap=app,.clients={app}};
    assert(sunstone_net_init(&net,&c));vTaskStartScheduler();platform_panic("scheduler",0);
}
