/* Only the changed DHCP/IP-task boundary executes. The existing authorized
 * radio/firmware checkpoint supplies both association generations; no WPA
 * handshake, reset or physical RF behavior is established here. */
#define kernel_main previous_network_kernel_main
#include "radio-network.c"
#undef kernel_main
#include "FreeRTOS_DHCP.h"
#include "FreeRTOS_IP_Utils.h"
#include "FreeRTOS_ARP.h"
atomic_uint reconnect_checkpoint_gate;
uint64_t reconnect_observation[16];
static atomic_uint client_ready,client_done;
static StaticTask_t reconnect_client_storage;
static StackType_t reconnect_client_stack[4096];
static TaskHandle_t reconnect_client_handle;
static unsigned silent_resume,discovers[3],acknowledgements[3],stale_injected;
static uint32_t transaction[3],random_counter;
static int reconnect_random(void *context,uint32_t *value)
{assert(context==&m);*value=0x13579bdfu+(++random_counter);return 1;}
static void dhcp_response(const uint8_t *request,unsigned bytes,unsigned type,uint32_t xid,unsigned address)
{
    uint8_t payload[300]={2,1,6};store32(payload+4,xid);memcpy(payload+16,local_ip,4);payload[19]=(uint8_t)address;
    memcpy(payload+20,peer_ip,4);memcpy(payload+28,local_mac,6);store32(payload+236,0x63825363);unsigned at=240;
#define OPTION(code,count) do{payload[at++]=(code);payload[at++]=(count);}while(0)
    OPTION(53,1);payload[at++]=(uint8_t)type;OPTION(54,4);memcpy(payload+at,peer_ip,4);at+=4;
    OPTION(51,4);store32(payload+at,120);at+=4;OPTION(1,4);memset(payload+at,255,3);at+=4;
    OPTION(3,4);memcpy(payload+at,peer_ip,4);at+=4;OPTION(6,4);memcpy(payload+at,peer_ip,4);at+=4;payload[at++]=255;
#undef OPTION
    assert(bytes>=42&&at<=sizeof(payload));uint8_t response[1514]={0};ip_header(response,8+at,17);
    memset(response+30,255,4);store16(response+24,0);store16(response+24,checksum(sum(response+14,20,0)));
    store16(response+34,67);store16(response+36,68);store16(response+38,(uint16_t)(8+at));memcpy(response+42,payload,at);
    store16(response+40,protocol_checksum(response+14,response+34,8+at,17));enqueue(response,42+at);
    (void)request;
}
static void reconnect_peer(const uint8_t *p,unsigned n)
{
    if(n<42||be16(p+12)!=0x800||p[23]!=17||be16(p+34)!=68||be16(p+36)!=67){peer(p,n);return;}
    unsigned bytes=be16(p+38);assert(bytes>=252&&42+bytes-8<=n);
    assert(!checksum(sum(p+14,20,0))&&!protocol_checksum(p+14,p+34,bytes,17));
    const uint8_t *body=p+42;unsigned length=bytes-8,type=0;
    assert(body[0]==1&&body[1]==1&&body[2]==6&&be32(body+236)==0x63825363&&!memcmp(body+28,local_mac,6));
    for(unsigned at=240;at<length;){unsigned option=body[at++];if(option==255)break;if(!option)continue;
        assert(at<length);unsigned count=body[at++];assert(count<=length-at);if(option==53){assert(count==1);type=body[at];}at+=count;}
    unsigned generation=(unsigned)wifi.association_generation;assert(generation==1||generation==2);assert(type==1||type==3);
    uint32_t xid=be32(body+4);
    if(type==1){++discovers[generation];transaction[generation]=xid;if(generation==2)assert(xid!=transaction[1]);}
    else{assert(xid==transaction[generation]);++acknowledgements[generation];}
    if(generation==2&&silent_resume)return;
    if(generation==2&&type==3&&!stale_injected){
        /* A delayed old ACK reaches the real fresh DHCP socket while it waits
         * for its new ACK; the old transaction must not publish authority. */
        dhcp_response(p,n,5,transaction[1],2);++stale_injected;
    }
    dhcp_response(p,n,type==1?2:5,xid,generation==1?2:3);
}
static int reconnect_write(void *context,uint32_t offset,uint32_t value)
{
    int ok=write_cb(context,offset,value);
    if(offset==0x200&&m.write&&m.position==m.bytes){
        assert(m.transmitted[2]==1);unsigned bytes=m.transmitted[4]|(unsigned)m.transmitted[5]<<8;
        assert(bytes<=1500&&bytes+32<=m.bytes);uint8_t ethernet[1514];memcpy(ethernet,m.transmitted+12,14);
        memcpy(ethernet+14,m.transmitted+32,bytes);reconnect_peer(ethernet,bytes+14);
    }
    return ok;
}
static void reconnect_client(void *unused)
{
    (void)unused;while(!atomic_load(&client_ready))vTaskDelay(1);
    uint64_t probe=open_socket(0,7399);
    struct sunstone_net_request r={.operation=SUNSTONE_NET_SEND,.handle=probe,.port=9000,.timeout=500,.bytes=5};
    memcpy(r.address,peer_ip,4);memcpy(r.payload,"prime",5);assert(call(&r).result==SUNSTONE_NET_OK);
    uint32_t ip;memcpy(&ip,peer_ip,4);MACAddress_t mac;NetworkEndPoint_t *endpoint=NULL;
    TickType_t end=xTaskGetTickCount()+500;
    while(eARPGetCacheEntry(&ip,&mac,&endpoint)!=eResolutionCacheHit){assert((int64_t)(end-xTaskGetTickCount())>0);vTaskDelay(1);}
    assert(endpoint==&net.network.endpoint&&!memcmp(mac.ucBytes,peer_mac,6));close_socket(probe);
    unsigned before=udp_packets;uint64_t socket=open_socket(0,7400);exchange(socket,"DHCP resumed");close_socket(socket);
    assert(udp_packets==before+1);
    atomic_store(&client_done,1);vTaskSuspend(NULL);
}
static void reconnect_owner(void *unused)
{
    (void)unused;struct sunstone_net_config c={.network={.transport=&transport,.tx_timeout=5000,.queue_timeout=5000,
        .random32=reconnect_random,.sequence=sequence,.entropy_context=&m,.entropy_record=1,
        .dhcp=1,.address_timeout=1500,.lease_cap=2000},.bootstrap=xTaskGetCurrentTaskHandle(),.clients={reconnect_client_handle}};
    assert(aic8800_network_storage_valid(&net,sizeof(net),&c.network));assert(sunstone_net_init_owned(&net,&c));assert(sunstone_net_activate(&net));
    struct aic8800_network *n=&net.network;TickType_t deadline=xTaskGetTickCount()+5000;
    while(!aic8800_network_address(n,NULL)||!aic8800_transport_attachment_ready(&transport)||n->active){
        assert((int64_t)(deadline-xTaskGetTickCount())>0);assert(sunstone_net_poll_owned(&net));vTaskDelay(1);
    }
    assert(discovers[1]&&acknowledgements[1]&&n->endpoint.xDHCPData.eDHCPState==eLeasedAddress);
    TickType_t old_start=atomic_load(&n->lease_started),old_ticks=atomic_load(&n->lease_ticks);
    reconnect_observation[0]=wifi.association_generation;reconnect_observation[1]=transaction[1];
    reconnect_observation[2]=old_start;reconnect_observation[3]=old_ticks;
    platform_puts("CHECKPOINT DHCP before offline\n");while(!atomic_load(&reconnect_checkpoint_gate))vTaskDelay(1);
    /* The checkpoint is drained: request before another owner poll can start IO. */
    assert(aic8800_transport_attachment_ready(&transport));assert(sunstone_net_offline(&net));
    deadline=xTaskGetTickCount()+500;
    while(!aic8800_network_offline_ready(n)){assert((int64_t)(deadline-xTaskGetTickCount())>0);vTaskDelay(1);}
    assert(atomic_load(&n->offline_ack)==atomic_load(&n->offline_request));
    assert(!n->endpoint.xDHCPData.xDHCPSocket&&!n->endpoint.xDHCP_RATimer.bActive);
    assert(n->endpoint.xDHCPData.eDHCPState==eNotUsingLeasedAddress&&!n->endpoint.bits.bEndPointUp);
    /* This event is sent after the fence and executes while DHCP is parked. */
    assert(xSendDHCPEvent(&n->endpoint)==pdPASS);vTaskDelay(5);
    assert(!n->endpoint.xDHCPData.xDHCPSocket&&!n->endpoint.xDHCP_RATimer.bActive);
    assert(n->endpoint.xDHCPData.eDHCPState==eNotUsingLeasedAddress&&!aic8800_network_address(n,NULL));
    /* The old native address lease expires during the bounded offline period;
     * this must neither authorize traffic nor revoke the new acquisition. */
    vTaskDelay(2200);assert((TickType_t)(xTaskGetTickCount()-old_start)>=old_ticks);
    assert(!atomic_load(&n->address_failed)&&!atomic_load(&n->failed)&&!aic8800_network_address(n,NULL));
    wifi.association_generation=2;security.generation=2;supplicant.generation=2;security.keys[0].generation=2;local_ip[3]=3;
    TickType_t resume_start=xTaskGetTickCount();assert(aic8800_network_resume(n));
    assert(!sunstone_net_resume(&net));deadline=resume_start+4000;
    while(!aic8800_network_address(n,NULL)&&!atomic_load(&n->address_failed)){
        assert((int64_t)(deadline-xTaskGetTickCount())>0);assert(sunstone_net_poll_owned(&net));vTaskDelay(1);
    }
    assert(n->address_started>=resume_start&&n->generation==2&&atomic_load(&n->resume_request)==atomic_load(&n->offline_ack));
    reconnect_observation[4]=n->address_started;reconnect_observation[5]=n->generation;
    reconnect_observation[6]=transaction[2];reconnect_observation[7]=atomic_load(&n->lease_started);
    if(silent_resume){
        assert(atomic_load(&n->address_failed)&&!aic8800_network_address(n,NULL)&&atomic_load(&net.offline));
        assert(!sunstone_net_resume(&net)&&!aic8800_network_resume(n)&&!n->leased_address&&!atomic_load(&n->lease_ticks));
        assert(!atomic_load(&n->failed)&&transaction[2]!=transaction[1]);
        platform_puts("PASS DHCP reconnect acquisition deadline preserves closed clients and failed address authority\n");platform_exit(0);
    }
    uint8_t ip[4];assert(aic8800_network_address(n,ip)&&!memcmp(ip,local_ip,4)&&ip[3]==3);
    assert(transaction[2]!=transaction[1]&&stale_injected==1&&discovers[2]&&acknowledgements[2]);
    assert(atomic_load(&n->lease_started)>=n->address_started&&n->leased_address==atomic_load(&n->address_value));
    assert(!atomic_load(&n->address_failed)&&atomic_load(&net.offline)&&sunstone_net_resume(&net));
    atomic_store(&client_ready,1);deadline=xTaskGetTickCount()+10000;
    while(!atomic_load(&client_done)){assert((int64_t)(deadline-xTaskGetTickCount())>0);assert(sunstone_net_poll_owned(&net));vTaskDelay(1);}
    assert(udp_packets>=1&&!atomic_load(&net.offline)&&!atomic_load(&n->failed));
    reconnect_observation[8]=atomic_load(&client_done);reconnect_observation[9]=udp_packets;
    platform_puts("PASS DHCP reconnect IP epoch: stopped old lease, stale event and ACK excluded, fresh transaction/address, resumed UDP and CLOSE\n");platform_exit(0);
}
void kernel_main(void)
{
    platform_init();checkpoint();host.config.ops.write32=reconnect_write;
    assert(aic8800_security_open(&security)&&transport.state==AIC8800_TRANSPORT_RUNTIME&&
        card.state==SDIO_CARD_READY&&host.state==SUNXI_SDIO_READY&&aic8800_transport_attachment_ready(&transport));
    char command[64]={0};uint64_t block[2]={(uintptr_t)command,sizeof(command)};
    register uint64_t operation __asm__("x0")=0x15;register uint64_t *arg __asm__("x1")=block;
    __asm__ volatile("hlt #0xf000":"+r"(operation):"r"(arg):"memory");
    if(!operation)for(unsigned i=0;i+7<sizeof(command);++i)if(!memcmp(command+i,"timeout",7))silent_resume=1;
    reconnect_client_handle=xTaskCreateStatic(reconnect_client,"dhcp-client",4096,NULL,4,reconnect_client_stack,&reconnect_client_storage);assert(reconnect_client_handle);
    assert(xTaskCreateStatic(reconnect_owner,"dhcp-owner",4096,NULL,5,app_stack,&app_task));
    vTaskStartScheduler();platform_panic("scheduler",0);
}
