/* Actual stack DHCP/DNS and retained copied clients over the existing SDIO peer.
 * Association, decrypted CCMP and entropy are nonphysical fixture inputs. */
#define kernel_main previous_network_kernel_main
#include "radio-network.c"
#undef kernel_main
#include "FreeRTOS_DNS.h"
#include "FreeRTOS_DHCP.h"
#include "FreeRTOS_IP_Utils.h"
#include "FreeRTOS_ARP.h"
#include <strings.h>
#include "a733_rtc_service.h"
static unsigned offers,acks,queries,renewals,silent_dhcp,static_mode,drop_ack,omit_lease_mode,omit_lease;
static const uint8_t answer_ip[4]={10,0,0,7};
static void reply_udp(const uint8_t *request,unsigned bytes,const uint8_t *payload,unsigned length,int dhcp)
{
    assert(bytes>=42&&length<=1400);const uint8_t *udp=request+34;
    uint8_t reply[1514]={0};ip_header(reply,8+length,17);
    if(dhcp){memset(reply+30,255,4);store16(reply+24,0);store16(reply+24,checksum(sum(reply+14,20,0)));}
    store16(reply+34,be16(udp+2));store16(reply+36,be16(udp));store16(reply+38,(uint16_t)(8+length));
    memcpy(reply+42,payload,length);store16(reply+40,protocol_checksum(reply+14,reply+34,8+length,17));enqueue(reply,42+length);
}
/* SNTP server at answer_ip, the address DNS returns for every name. It
 * answers ARP for that address, then for each request first sends a datagram
 * from another port and one for an earlier nonce, which the client ignores.
 * The first DNS query goes unanswered, as when an ARP miss consumes it. */
static unsigned sntp_mode,sntp_requests,sntp_answered,sntp_dns_dropped;
static const uint8_t sntp_seconds[4]={0xee,0x83,0x2f,0xf0}; /* 2026-10-21 12:34:56 UTC */
static void sntp_datagram(const uint8_t *request,unsigned source_port,const uint8_t *body)
{
    uint8_t reply[90]={0};ip_header(reply,8+48,17);uint8_t *ip=reply+14,*udp=reply+34;
    memcpy(ip+12,answer_ip,4);store16(ip+10,0);store16(ip+10,checksum(sum(ip,20,0)));
    store16(udp,(uint16_t)source_port);store16(udp+2,be16(request+34));store16(udp+4,8+48);
    memcpy(udp+8,body,48);store16(udp+6,protocol_checksum(ip,udp,8+48,17));enqueue(reply,90);
}
static int sntp_peer(const uint8_t *p,unsigned n)
{
    if(n>=42&&be16(p+12)==0x806&&be16(p+20)==1&&!memcmp(p+38,answer_ip,4)){
        uint8_t reply[42]={0};memcpy(reply,local_mac,6);memcpy(reply+6,peer_mac,6);store16(reply+12,0x806);
        memcpy(reply+14,p+14,6);store16(reply+20,2);memcpy(reply+22,peer_mac,6);memcpy(reply+28,answer_ip,4);
        memcpy(reply+32,local_mac,6);memcpy(reply+38,local_ip,4);enqueue(reply,sizeof(reply));return 1;
    }
    if(n>=42&&be16(p+12)==0x800&&p[23]==17&&be16(p+36)==53&&!sntp_dns_dropped){sntp_dns_dropped=1;return 1;}
    if(n<42||be16(p+12)!=0x800||p[23]!=17||memcmp(p+30,answer_ip,4))return 0;
    const uint8_t *udp=p+34;assert(be16(udp+2)==123&&be16(udp+4)==8+48&&!protocol_checksum(p+14,udp,8+48,17));
    const uint8_t *q=udp+8;assert(q[0]==0x23&&!q[1]&&!memcmp(q+1,(uint8_t[39]){0},39));++sntp_requests;
    uint8_t body[48]={0x24,2,6,0xec};memcpy(body+24,q+40,8);
    /* Received at .5 s less 2^-16 s (15 us); sent at .5 s. */
    memcpy(body+32,sntp_seconds,4);body[36]=0x7f;body[37]=0xff;
    memcpy(body+40,sntp_seconds,4);body[44]=0x80;
    sntp_datagram(p,124,body);
    body[31]^=1;sntp_datagram(p,123,body);body[31]^=1;
    sntp_datagram(p,123,body);++sntp_answered;return 1;
}
static void service_peer(const uint8_t *p,unsigned n)
{
    if(sntp_mode&&sntp_peer(p,n))return;
    if(n<42||be16(p+12)!=0x800||p[23]!=17){peer(p,n);return;}
    const uint8_t *udp=p+34;unsigned bytes=be16(udp+4);assert(bytes>=8&&42+bytes-8<=n);
    assert(!checksum(sum(p+14,20,0))&&!protocol_checksum(p+14,udp,bytes,17));
    const uint8_t *body=udp+8;unsigned length=bytes-8;
    if(be16(udp)==68&&be16(udp+2)==67){
        assert(length>=244&&body[0]==1&&body[1]==1&&body[2]==6&&be32(body+236)==0x63825363&&!memcmp(body+28,local_mac,6));
        unsigned type=0;
        for(unsigned at=240;at<length;){unsigned option=body[at++];if(option==255)break;if(!option)continue;assert(at<length);unsigned count=body[at++];assert(count<=length-at);if(option==53){assert(count==1);type=body[at];}at+=count;}
        assert(type==1||type==3);if(silent_dhcp)return;
        if(type==1)++offers;else{++acks;if(acks>1)++renewals;if(drop_ack)return;}
        uint8_t out[300]={2,1,6};memcpy(out+4,body+4,4);memcpy(out+16,local_ip,4);memcpy(out+20,peer_ip,4);memcpy(out+28,local_mac,6);store32(out+236,0x63825363);
        unsigned at=240;
#define OPTION(code,count) do{out[at++]=(code);out[at++]=(count);}while(0)
        OPTION(53,1);out[at++]=type==1?2:5;OPTION(54,4);memcpy(out+at,peer_ip,4);at+=4;
        if(!(omit_lease&&type==3)){OPTION(51,4);store32(out+at,120);at+=4;}OPTION(1,4);memset(out+at,255,3);at+=4;
        OPTION(3,4);memcpy(out+at,peer_ip,4);at+=4;OPTION(6,4);memcpy(out+at,peer_ip,4);at+=4;out[at++]=255;
#undef OPTION
        reply_udp(p,n,out,at,1);return;
    }
    if(be16(udp+2)==53){
        assert(length>=17&&be16(body+4)==1);++queries;
        unsigned at=12;char name[254];unsigned used=0;
        while(at<length&&body[at]){unsigned count=body[at++];assert(count<=63&&count<=length-at&&used+count+1<sizeof(name));if(used)name[used++]='.';memcpy(name+used,body+at,count);used+=count;at+=count;}
        name[used]=0;assert(at+5==length&&be16(body+at+1)==1&&be16(body+at+3)==1);
        if(strlen(name)==11&&!memcmp(name,"silent.test",11))return;
        uint8_t out[512]={0};assert(length+16<=sizeof(out));memcpy(out,body,length);store16(out+2,0x8180);store16(out+6,1);
        at=length;out[at++]=0xc0;out[at++]=12;store16(out+at,1);at+=2;store16(out+at,1);at+=2;store32(out+at,60);at+=4;store16(out+at,4);at+=2;memcpy(out+at,answer_ip,4);at+=4;
        if(strlen(name)==10&&!memcmp(name,"wrong.test",10))out[0]^=1;
        reply_udp(p,n,out,at,0);return;
    }
    peer(p,n);
}
static int service_write(void *context,uint32_t offset,uint32_t value)
{
    int ok=write_cb(context,offset,value);
    if(offset==0x200&&m.write&&m.position==m.bytes){
        unsigned bytes=m.transmitted[4]|(unsigned)m.transmitted[5]<<8;assert(bytes<=1500&&bytes+32<=m.bytes);
        uint8_t ethernet[1514];memcpy(ethernet,m.transmitted+12,14);memcpy(ethernet+14,m.transmitted+32,bytes);service_peer(ethernet,bytes+14);
    }
    return ok;
}
static struct sunstone_net_reply resolve(const char *name,TickType_t timeout)
{
    struct sunstone_net_request r={.operation=SUNSTONE_NET_RESOLVE,.timeout=timeout,.bytes=(uint16_t)strlen(name)};
    memcpy(r.payload,name,r.bytes);struct sunstone_net_reply value=call(&r);
    platform_puts("DNS ");platform_puts(name);platform_puts(" result/queries/state/failed/tick ");
    platform_hex(value.result);platform_hex(queries);platform_hex(atomic_load(&net.dns[0].state));
    platform_hex(atomic_load(&net.network.address_failed));platform_hex(xTaskGetTickCount());platform_puts("\n");
    if(value.result!=SUNSTONE_NET_OK){
        platform_puts("transport state/error/pending/held/peer/rx/tx/callback/start ");
        platform_hex(transport.state);platform_hex(transport.error);platform_hex(transport.pending);platform_hex(transport.held);
        platform_hex(peer_count);platform_hex(net.network.rx_packets);platform_hex(net.network.tx_bus_completions);
        platform_hex(atomic_load(&net.dns[0].callback_ready));platform_hex(atomic_load(&net.network.lease_started));platform_puts("\n");
    }
    return value;
}
static void wait_state(atomic_uint *state,unsigned expected)
{for(unsigned i=0;i<1000&&atomic_load(state)!=expected;++i)vTaskDelay(1);assert(atomic_load(state)==expected);}
static unsigned sntp_random_calls;
static int sntp_random(void *context,uint8_t *bytes,size_t count)
{assert(context==&m&&count==8);++sntp_random_calls;for(size_t i=0;i<count;++i)bytes[i]=(uint8_t)(0x31*sntp_random_calls+i);return 1;}
/* Name resolution, ARP misses for the DNS and time servers, ignored foreign
 * datagrams, and the RTC request derived from the accepted reply. */
static void sntp_application(void)
{
    struct sunstone_sntp_config config={.random=sntp_random,.random_context=&m,.timeout=200,.attempts=4,
        .max_round_trip_us=1000000,.name_bytes=9,.name="time.test"};
    uint64_t start=(uint64_t)xTaskGetTickCount()*1000;config.deadline_us=start+20000000;
    static struct sunstone_sntp_client client;
    struct sunstone_sntp_config bad=config;bad.attempts=0;assert(!sunstone_sntp_start(&client,&net,&bad));
    bad=config;memcpy(bad.server,peer_ip,4);assert(!sunstone_sntp_start(&client,&net,&bad));
    assert(sunstone_sntp_start(&client,&net,&config));
    int result;unsigned polls=0;
    while(!(result=sunstone_sntp_poll(&client,(uint64_t)xTaskGetTickCount()*1000))){assert(++polls<30000);vTaskDelay(1);}
    platform_puts("SNTP result/queries/requests/sent/random/round trip ");platform_hex((unsigned)result);platform_hex(queries);
    platform_hex(sntp_requests);platform_hex(client.sent);platform_hex(sntp_random_calls);platform_hex((unsigned)client.time.round_trip_us);platform_puts("\n");
    assert(result==1&&!memcmp(client.server,answer_ip,4)&&sntp_answered&&client.sent==sntp_random_calls);
    assert(sntp_dns_dropped&&queries>=1);
    uint64_t server=UINT64_C(1792586096)*1000000u+500000u;
    assert(client.time.stratum==2&&!client.time.leap&&client.time.round_trip_us<=1000000&&
        client.time.unix_us==server+client.time.round_trip_us/2&&client.time.local_us>=start&&client.sent>=2);
    /* The socket is closed: every ledger entry is free again. */
    for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)assert(!net.sockets[i].socket);
    struct rtc_request rtc;
    assert(!sunstone_sntp_rtc_request(&client.time,client.time.local_us-1,&rtc));
    assert(sunstone_sntp_rtc_request(&client.time,client.time.local_us+3600000000u-client.time.round_trip_us/2,&rtc));
    assert(rtc.kind==RTC_REQUEST_SET&&rtc.day==20748&&rtc.hms==(13u<<16|34u<<8|56u)&&!rtc.seconds);
    platform_puts("PASS actual-stack SNTP: repeated lookup after an unanswered DNS query, request resent after an ARP miss, ignored foreign datagrams, close and RTC request\n");
    platform_exit(0);
}
static void address_application(void *unused)
{
    (void)unused;assert(sunstone_net_activate(&net));
    struct sunstone_net_request status={.operation=SUNSTONE_NET_ADDRESS,.timeout=2500};struct sunstone_net_reply value=call(&status);
    if(sntp_mode){assert(value.result==SUNSTONE_NET_OK);sntp_application();}
    if(silent_dhcp){
        assert(value.result==SUNSTONE_NET_ADDRESS_LOST&&atomic_load(&net.network.address_failed)&&aic8800_network_ready(&net.network));
        assert(!aic8800_network_address(&net.network,NULL));platform_puts("PASS DHCP acquisition deadline revokes address without inventing link loss\n");platform_exit(0);
    }
    assert(value.result==SUNSTONE_NET_OK&&value.bytes==16&&!memcmp(value.address,local_ip,4)&&!memcmp(value.payload+12,peer_ip,4));
    if(!static_mode){
        assert(offers&&acks);unsigned old=acks,sequence=atomic_load(&net.network.address_sequence);
        TickType_t start=atomic_load(&net.network.lease_started);vTaskDelay(2);omit_lease=omit_lease_mode;
        assert(xSendDHCPEvent(&net.network.endpoint)==pdPASS);
        for(unsigned i=0;i<1500&&atomic_load(&net.network.address_sequence)==sequence&&!atomic_load(&net.network.address_failed);++i)vTaskDelay(1);
        assert(acks>old&&renewals);
        if(omit_lease_mode){
            assert(atomic_load(&net.network.address_failed)&&atomic_load(&net.network.address_sequence)==sequence&&atomic_load(&net.network.lease_started)==start);
            platform_puts("PASS DHCP ACK without fresh lease cannot renew native authority\n");platform_exit(0);
        }
        assert(atomic_load(&net.network.address_sequence)>sequence&&!(atomic_load(&net.network.address_sequence)&1)&&
            atomic_load(&net.network.lease_started)>start&&aic8800_network_address(&net.network,NULL));
        platform_puts("PASS actual stable DHCP lease acquisition and renewal publication\n");
    }
    /* Prime the real ARP path before DNS's intentionally single nonblocking send. */
    uint64_t udp=open_socket(0,7400);struct sunstone_net_request prime={.operation=SUNSTONE_NET_SEND,.handle=udp,.timeout=500,.bytes=5,.port=9000};
    memcpy(prime.address,peer_ip,4);memcpy(prime.payload,"prime",5);assert(call(&prime).result==SUNSTONE_NET_OK);
    uint32_t arp_ip;memcpy(&arp_ip,peer_ip,4);MACAddress_t arp_mac;NetworkEndPoint_t *arp_endpoint=NULL;
    unsigned arp_wait;
    for(arp_wait=0;arp_wait<500;++arp_wait){
        if(eARPGetCacheEntry(&arp_ip,&arp_mac,&arp_endpoint)==eResolutionCacheHit)break;
        vTaskDelay(1);
    }
    assert(arp_wait<500&&arp_endpoint==&net.network.endpoint&&!memcmp(arp_mac.ucBytes,peer_mac,6));close_socket(udp);
    value=resolve("answer.test",500);assert(value.result==SUNSTONE_NET_OK&&!memcmp(value.address,answer_ip,4)&&queries);
    value=resolve("wrong.test",40);assert(value.result==SUNSTONE_NET_TIMEOUT||value.result==SUNSTONE_NET_IO);
    for(unsigned i=0;i<1000&&atomic_load(&net.dns[0].state)!=SUNSTONE_DNS_CANCELED&&atomic_load(&net.dns[0].state)!=SUNSTONE_DNS_IDLE;++i)vTaskDelay(1);
    assert(atomic_load(&net.dns[0].state)==SUNSTONE_DNS_CANCELED||atomic_load(&net.dns[0].state)==SUNSTONE_DNS_IDLE);
    /* A real registered query is canceled while the IP task cannot acknowledge.
     * Its immutable slot remains busy, then is reused only after the fence. */
    struct sunstone_net_request q={.operation=SUNSTONE_NET_RESOLVE,.timeout=500,.bytes=11};memcpy(q.payload,"silent.test",11);
    uint64_t ticket;assert(sunstone_net_submit(&net,&q,&ticket));wait_state(&net.dns[0].state,SUNSTONE_DNS_WAIT);
    TaskHandle_t ip=FreeRTOS_GetIPTaskHandle();vTaskSuspend(ip);assert(sunstone_net_cancel(&net,ticket));
    assert(sunstone_net_receive(&net,&value,100)&&value.result==SUNSTONE_NET_CANCELED&&net.dns[0].ticket==ticket);
    value=resolve("answer.test",100);assert(value.result==SUNSTONE_NET_BUSY&&net.dns[0].ticket==ticket);
    vTaskResume(ip);wait_state(&net.dns[0].state,SUNSTONE_DNS_CANCELED);
    value=resolve("answer.test",500);assert(value.result==SUNSTONE_NET_OK&&!memcmp(value.address,answer_ip,4));
    if(!static_mode){
        drop_ack=1;assert(xSendDHCPEvent(&net.network.endpoint)==pdPASS);
        /* Explicitly model a preempted IP-task publication. An unstable
         * snapshot denies admission, but is not itself permanent expiry. */
        vTaskSuspend(ip);unsigned sequence=atomic_fetch_add(&net.network.address_sequence,1);assert(!(sequence&1));
        assert(!aic8800_network_address(&net.network,NULL));
        vTaskDelay(2100);assert(!aic8800_network_address(&net.network,NULL)&&!atomic_load(&net.network.address_failed));
        atomic_fetch_add(&net.network.address_sequence,1);vTaskResume(ip);
        for(unsigned i=0;i<100&&!atomic_load(&net.network.address_failed);++i)vTaskDelay(1);
        assert(atomic_load(&net.network.address_failed)&&aic8800_network_ready(&net.network));
        value=resolve("answer.test",100);assert(value.result==SUNSTONE_NET_ADDRESS_LOST);
        assert(!atomic_load(&net.network.failed));
    }
    platform_puts("PASS actual DHCP acquisition/renewal or fixed IPv4, DNS parser, mismatched-ID refusal, cancel fence and address expiry\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();checkpoint();host.config.ops.write32=service_write;
    char command[128]={0};uint64_t block[2]={(uintptr_t)command,sizeof(command)};
    register uint64_t operation __asm__("x0")=0x15;register uint64_t *arg __asm__("x1")=block;
    __asm__ volatile("hlt #0xf000":"+r"(operation):"r"(arg):"memory");
    if(!operation)for(unsigned i=0;i+11<sizeof(command);++i){if(!memcmp(command+i,"acquisition",11))silent_dhcp=1;if(!memcmp(command+i,"static",6))static_mode=1;if(!memcmp(command+i,"omitted",7))omit_lease_mode=1;if(!memcmp(command+i,"sntp",4))sntp_mode=static_mode=1;}
    TaskHandle_t app=xTaskCreateStatic(address_application,"address-app",4096,NULL,5,app_stack,&app_task);assert(app);
    struct sunstone_net_config c={.network={.transport=&transport,.tx_timeout=5000,.queue_timeout=5000,
        .random32=random32,.sequence=sequence,.entropy_context=&m,.entropy_record=1,
        .address={10,0,0,2},.netmask={255,255,255,0},.gateway={10,0,0,1},.dns={10,0,0,1},
        .dhcp=!static_mode,.address_timeout=1000,.lease_cap=2000},.bootstrap=app,.clients={app}};
    /* Preflight the one physical-boundary fixture before clients or IP tasks:
     * actual association ownership, idle controller, immutable entropy context,
     * disjoint copied service/config and finite address/lease geometry. */
    unsigned setup_failures=0;
    if(!(transport.state==AIC8800_TRANSPORT_RUNTIME&&wifi.association_generation==1&&aic8800_security_open(&security)))setup_failures|=1;
    if(!(card.state==SDIO_CARD_READY&&host.state==SUNXI_SDIO_READY&&!transport.pending&&!transport.data_tx&&!transport.security_tx))setup_failures|=2;
    if(!(aic8800_network_storage_valid(&net,sizeof(net),&c.network)&&c.network.address_timeout&&c.network.lease_cap<=portMAX_DELAY/2))setup_failures|=4;
    char copy[5]={0x55,0x55,0x55,0x55,0x55};
    if(strcpy(copy,"xy")!=copy||memcmp(copy,"xy\0",3)||copy[3]!=0x55)setup_failures|=8;
    if(strncpy(copy,"a",5)!=copy||memcmp(copy,"a\0\0\0\0",5))setup_failures|=16;
    memset(copy,0x55,sizeof(copy));strncpy(copy,"abcd",3);
    if(memcmp(copy,"abc",3)||copy[3]!=0x55||strcasecmp("aZ","Az")||strcasecmp("A","b")>=0||strcasecmp("b","A")<=0)setup_failures|=32;
    const char high[2]={(char)0x80,0},text[]="abc";
    if(strcmp("same","same")||strcmp("ab","abc")>=0||strcmp(high,"\x7f")<=0||
        strchr(text,'b')!=text+1||strchr(text,0)!=text+3||strchr(text,'z')||strchr(text,0x162)!=text+1)setup_failures|=64;
    if(setup_failures){platform_puts("fixture setup failures ");platform_hex(setup_failures);platform_panic("network baseline",0);}
    assert(sunstone_net_init(&net,&c));vTaskStartScheduler();platform_panic("scheduler",0);
}
