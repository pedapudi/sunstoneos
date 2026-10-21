#include "aic8800_network.h"
#include <limits.h>
#include "aic8800_data.h"
#include "aic8800_security.h"
#include "aic8800_supplicant.h"
#include "aic8800_runtime.h"
#include "aic8800_firmware.h"
#include "FreeRTOS_IP_Private.h"
#include "FreeRTOS_DHCP.h"
#include "NetworkBufferManagement.h"
#include "task.h"
#include <string.h>
static struct aic8800_network *stack_owner;
static _Alignas(16) uint8_t network_buffers[ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS][1536];
_Static_assert(ipBUFFER_PADDING+ipconfigNETWORK_MTU+ipSIZE_OF_ETH_HEADER<=1536,"network pool capacity");
static int disjoint(const void *p,size_t bytes,const void *q,size_t other)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)q;
    if(!p||bytes>UINTPTR_MAX-a)return 0;
    if(!q)return 1;
    return other<=UINTPTR_MAX-b&&!(a<b+other&&b<a+bytes);
}
int aic8800_network_storage_valid(const void *p,size_t bytes,const struct aic8800_network_config *c)
{
    if(!c||!c->transport||!c->transport->initialized)return 0;
    const struct aic8800_sdio_transport *t=c->transport;
    if(t->firmware)for(unsigned i=0;i<AIC8800_FIRMWARE_BLOBS;++i)
        if(!disjoint(p,bytes,t->firmware->config.blobs[i].data,t->firmware->config.blobs[i].bytes))return 0;
    return disjoint(p,bytes,t->receive_owner,t->receive_owner_bytes)&&disjoint(p,bytes,t,sizeof(*t))&&t->card&&disjoint(p,bytes,t->card,sizeof(*t->card))&&
        t->card->host&&disjoint(p,bytes,t->card->host,sizeof(*t->card->host))&&
        disjoint(p,bytes,t->data,sizeof(struct aic8800_data))&&disjoint(p,bytes,t->wifi,sizeof(struct aic8800_wifi))&&
        disjoint(p,bytes,t->security,sizeof(struct aic8800_security))&&disjoint(p,bytes,t->supplicant,sizeof(struct aic8800_supplicant))&&
        disjoint(p,bytes,t->runtime,sizeof(struct aic8800_runtime))&&disjoint(p,bytes,t->firmware,sizeof(struct aic8800_firmware));
}
static int authority_for(const struct aic8800_sdio_transport *t,uint64_t generation)
{
    if(!t||!t->wifi||!t->data||t->state!=AIC8800_TRANSPORT_RUNTIME||t->wifi->state!=AIC8800_WIFI_READY||
        t->wifi->association_generation!=generation)return 0;
    if(t->wifi->link==AIC8800_WIFI_ASSOCIATED_OPEN)
        return t->data->open_authority&&t->data->open_generation==generation;
    return t->wifi->link==AIC8800_WIFI_SECURED&&t->supplicant&&
        t->supplicant->state==AIC8800_SUPPLICANT_AUTHORIZED&&aic8800_security_open(t->security);
}
int aic8800_network_ready(const struct aic8800_network *n)
{return n&&n->initialized&&!atomic_load(&n->offline)&&!atomic_load(&n->failed)&&atomic_load(&n->ready);}
static int address_expired(const struct aic8800_network *n)
{
    if(!n->config.dhcp||atomic_load_explicit(&n->offline,memory_order_acquire))return 0;
    unsigned first=atomic_load(&n->address_sequence);if(first&1)return -1;
    TickType_t start=atomic_load(&n->lease_started),limit=atomic_load(&n->lease_ticks);
    if(!atomic_load(&n->address_ready)){start=atomic_load(&n->address_started);limit=n->config.address_timeout;}
    int expired=(TickType_t)(xTaskGetTickCount()-start)>=limit;
    return first==atomic_load(&n->address_sequence)?expired:-1;
}
int aic8800_network_address(const struct aic8800_network *n,uint8_t value[4])
{
    if(!aic8800_network_ready(n)||!atomic_load(&n->address_ready)||atomic_load(&n->address_failed)||address_expired(n)!=0)return 0;
    uint32_t address=atomic_load(&n->address_value);if(value)memcpy(value,&address,4);return 1;
}
int aic8800_network_status(const struct aic8800_network *n,uint32_t values[4])
{
    if(!aic8800_network_address(n,NULL))return 0;
    unsigned first=atomic_load(&n->address_sequence);if(first&1)return 0;
    values[0]=atomic_load(&n->address_value);values[1]=atomic_load(&n->mask_value);
    values[2]=atomic_load(&n->gateway_value);values[3]=atomic_load(&n->dns_value);
    return first==atomic_load(&n->address_sequence)&&aic8800_network_address(n,NULL);
}
static void address_revoke(struct aic8800_network *n)
{atomic_store(&n->address_ready,0);atomic_store(&n->address_failed,1);}
void aic8800_network_ip_step(void)
{
    struct aic8800_network *n=stack_owner;if(!n||!n->config.dhcp)return;
    if(atomic_load(&n->failed)||atomic_load(&n->address_failed)||address_expired(n)==1)address_revoke(n);
    /* The stack has already copied this lease into its renewal timer. Clear
     * the retained option before a later ACK can reuse it. Only a freshly
     * parsed option 51 may authorize another native lease publication. */
    if(n->endpoint.xDHCPData.eDHCPState==eLeasedAddress)n->endpoint.xDHCPData.ulLeaseTime=0;
}
eDHCPCallbackAnswer_t xApplicationDHCPHook_Multi(eDHCPCallbackPhase_t phase,NetworkEndPoint_t *endpoint,IP_Address_t *address)
{
    (void)phase;(void)address;struct aic8800_network *n=stack_owner;
    if(!n||endpoint!=&n->endpoint||!n->config.dhcp||atomic_load(&n->offline)||atomic_load(&n->failed)||atomic_load(&n->address_failed))return eDHCPStopNoChanges;
    if(address_expired(n)==1)address_revoke(n);
    if(phase==eDHCPPhasePreRequest)endpoint->xDHCPData.ulLeaseTime=0;
    return atomic_load(&n->address_failed)?eDHCPStopNoChanges:eDHCPContinue;
}
void vApplicationIPNetworkEventHook_Multi(eIPCallbackEvent_t event,NetworkEndPoint_t *endpoint)
{
    struct aic8800_network *n=stack_owner;if(!n||endpoint!=&n->endpoint)return;
    if(event!=eNetworkUp){atomic_store(&n->address_ready,0);return;}
    if(address_expired(n)==1)address_revoke(n);
    if(atomic_load(&n->offline)||atomic_load(&n->failed)||atomic_load(&n->address_failed))return;
    uint32_t ip=endpoint->ipv4_settings.ulIPAddress;
    if(n->config.dhcp){
        /* The hook precedes the stack's default/minimum lease substitution.
         * Use only the server-supplied half-lease, never an invented default. */
        uint32_t half=endpoint->xDHCPData.ulLeaseTime;
        if(endpoint->xDHCPData.eDHCPState!=eLeasedAddress||!half||!ip||
            (n->leased_address&&n->leased_address!=ip)){address_revoke(n);return;}
        uint64_t ticks=2*(uint64_t)half;
        atomic_fetch_add(&n->address_sequence,1);
        atomic_store(&n->lease_ticks,ticks<n->config.lease_cap?(TickType_t)ticks:n->config.lease_cap);
        atomic_store(&n->lease_started,xTaskGetTickCount());n->leased_address=ip;
    }else atomic_fetch_add(&n->address_sequence,1);
    atomic_store(&n->address_value,ip);atomic_store(&n->mask_value,endpoint->ipv4_settings.ulNetMask);
    atomic_store(&n->gateway_value,endpoint->ipv4_settings.ulGatewayAddress);
    atomic_store(&n->dns_value,endpoint->ipv4_settings.ulDNSServerAddresses[0]);
    atomic_fetch_add(&n->address_sequence,1);atomic_store(&n->address_ready,ip!=0);
}
static int dhcp_packet(const uint8_t *p,size_t bytes)
{
    if(bytes<42||p[12]!=8||p[13]!=0||p[14]!=0x45||p[23]!=17)return 0;
    return p[34]==0&&p[35]==68&&p[36]==0&&p[37]==67;
}
void aic8800_network_fault(struct aic8800_network *n)
{if(n&&n->initialized){atomic_store(&n->ready,0);atomic_store(&n->failed,1);}}
static BaseType_t link_status(NetworkInterface_t *interface)
{return aic8800_network_ready(interface->pvArgument)?pdTRUE:pdFALSE;}
static BaseType_t output(NetworkInterface_t *interface,NetworkBufferDescriptor_t *const descriptor,BaseType_t release)
{
    struct aic8800_network *n=interface->pvArgument;BaseType_t result=pdFAIL;
    if(descriptor&&descriptor->pucEthernetBuffer&&descriptor->xDataLength>=14&&descriptor->xDataLength<=1514&&aic8800_network_ready(n)) {
        const uint8_t *p=descriptor->pucEthernetBuffer;
        if(!(p[12]==0x88&&p[13]==0x8e)&&(aic8800_network_address(n,NULL)||
            (n->config.dhcp&&!atomic_load(&n->address_failed)&&address_expired(n)==0&&dhcp_packet(p,descriptor->xDataLength)))) {
            struct aic8800_network_packet packet={.generation=n->generation,.queued=xTaskGetTickCount(),.bytes=(uint16_t)descriptor->xDataLength};
            memcpy(packet.ethernet,p,packet.bytes);result=xQueueSend(n->tx,&packet,0);
        }
    }
    if(descriptor&&release!=pdFALSE)vReleaseNetworkBufferAndDescriptor(descriptor);
    return result;
}
size_t uxNetworkInterfaceAllocateRAMToBuffers(NetworkBufferDescriptor_t descriptors[ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS])
{
    for(unsigned i=0;i<ipconfigNUM_NETWORK_BUFFER_DESCRIPTORS;++i){
        NetworkBufferDescriptor_t *p=&descriptors[i];memcpy(network_buffers[i],&p,sizeof(p));
        p->pucEthernetBuffer=network_buffers[i]+ipBUFFER_PADDING;
    }
    return sizeof(network_buffers[0])-ipBUFFER_PADDING;
}
BaseType_t xApplicationGetRandomNumber(uint32_t *value)
{
    struct aic8800_network *n=stack_owner;
    int r=n&&!atomic_load(&n->failed)&&value?n->config.random32(n->config.entropy_context,value):-1;
    if(r==1)return pdTRUE;
    /* The stack handles pdFALSE per use, so a temporary shortage or contention
     * (0) fails only this call. Any other refusal closes output. */
    if(value)*value=0;
    if(r!=0)aic8800_network_fault(n);
    return pdFALSE;
}
uint32_t ulApplicationGetNextSequenceNumber(uint32_t source,uint16_t sport,uint32_t destination,uint16_t dport)
{
    uint32_t value=0;struct aic8800_network *n=stack_owner;
    if(!n||atomic_load(&n->failed)||n->config.sequence(n->config.entropy_context,source,sport,destination,dport,&value)!=1)
        aic8800_network_fault(n);
    /* The stack hook has no failure result. Failed entropy closes pfOutput
     * synchronously, so this placeholder can never authorize an outgoing SYN. */
    return value;
}
int aic8800_network_activate(struct aic8800_network *n,const struct aic8800_network_config *c)
{
    if(!n||!c||n->initialized||stack_owner||!aic8800_network_storage_valid(n,sizeof(*n),c)||!c->transport||!c->transport->wifi||!c->transport->data||
        !c->tx_timeout||!c->queue_timeout||c->dhcp>1||
        (c->dhcp&&(!c->address_timeout||c->address_timeout>portMAX_DELAY/2||!c->lease_cap||c->lease_cap>portMAX_DELAY/2))||!c->entropy_record||!c->random32||!c->sequence)return 0;
    if(c->transport->receive_owner){
        if(!c->dispatcher||c->transport->receive_owner!=c->dispatcher||
            c->transport->receive_owner_bytes!=sizeof(*c->dispatcher)||!c->dispatcher->initialized||
            c->dispatcher->faulted||c->dispatcher->entered||c->dispatcher->transport!=c->transport)return 0;
    }else if(c->dispatcher)return 0;
    uint64_t generation=c->transport->wifi->association_generation;
    if(!generation||!authority_for(c->transport,generation)||
        c->transport->entered||c->transport->pending||c->transport->data_tx||c->transport->security_tx||
        c->transport->data->state!=AIC8800_DATA_IDLE||c->transport->card->state!=SDIO_CARD_READY||
        c->transport->card->host->state!=SUNXI_SDIO_READY)return 0;
    uintptr_t begin=(uintptr_t)n,end=(uintptr_t)c;
    if(sizeof(*n)>UINTPTR_MAX-begin||sizeof(*c)>UINTPTR_MAX-end||
        (begin<end+sizeof(*c)&&end<begin+sizeof(*n)))return 0;
    memset(n,0,sizeof(*n));n->initialized=1;n->config=*c;n->generation=generation;
    if(c->dispatcher)n->receive=c->dispatcher;
    else{
        if(!aic8800_dispatch_init(&n->local_dispatcher,c->transport)){aic8800_network_fault(n);return 0;}
        n->receive=&n->local_dispatcher;
    }
    n->tx=xQueueCreateStatic(AIC8800_NETWORK_TX_SLOTS,sizeof(struct aic8800_network_packet),n->tx_storage,&n->tx_queue);
    if(!n->tx){aic8800_network_fault(n);return 0;}
    n->interface=(NetworkInterface_t){.pcName="wifi",.pvArgument=n,.pfInitialise=link_status,.pfOutput=output,.pfGetPhyLinkStatus=link_status};
    FreeRTOS_AddNetworkInterface(&n->interface);
    FreeRTOS_FillEndPoint(&n->interface,&n->endpoint,c->address,c->netmask,c->gateway,c->dns,c->transport->wifi->mac);
    n->endpoint.bits.bWantDHCP=c->dhcp;atomic_store(&n->address_started,xTaskGetTickCount());
    stack_owner=n;atomic_store(&n->ready,1);
    if(FreeRTOS_IPInit_Multi()!=pdPASS){aic8800_network_fault(n);return 0;}n->stack_started=1;return 1;
}
void aic8800_network_poll(struct aic8800_network *n)
{
    if(!n||!n->initialized)return;
    struct aic8800_sdio_transport *t=n->config.transport;
    if(address_expired(n)==1)address_revoke(n);
    if(atomic_load(&n->failed)){if(n->stack_started&&!n->down_sent){FreeRTOS_NetworkDown(&n->interface);n->down_sent=1;}return;}
    if(atomic_load(&n->offline))return;
    aic8800_dispatch_poll(n->receive);
    if(n->receive->faulted||t->state==AIC8800_TRANSPORT_FAULT||!t->wifi||t->wifi->association_generation!=n->generation||
        t->wifi->link==AIC8800_WIFI_LINK_DOWN||t->wifi->link==AIC8800_WIFI_LINK_UNRESOLVED){aic8800_network_fault(n);return;}
    atomic_store(&n->ready,authority_for(t,n->generation));
    if(n->active&&t->data->state==AIC8800_DATA_DONE){
        struct aic8800_data_result result;
        if(aic8800_transport_take_data_result(t,&result)){
            if(result.eapol||!result.bus_transferred){aic8800_network_fault(n);return;}
            n->active=0;n->owns_current=0;++n->tx_bus_completions;
        }
    }
    if(t->held){
        struct aic8800_data_frame frame;
        if(!aic8800_dispatch_peek_data(n->receive,&frame))return; /* Other dispatcher retains raw/control ownership. */
        if(frame.generation!=n->generation){aic8800_network_fault(n);return;}
        NetworkBufferDescriptor_t *buffer=aic8800_network_ready(n)?pxGetNetworkBufferWithDescriptor(frame.bytes,0):NULL;
        if(buffer){
            memcpy(buffer->pucEthernetBuffer,frame.ethernet,frame.bytes);buffer->xDataLength=frame.bytes;
            buffer->pxInterface=&n->interface;buffer->pxEndPoint=&n->endpoint;
            IPStackEvent_t event={eNetworkRxEvent,buffer};
            if(xSendEventStructToIPTask(&event,0)==pdPASS)++n->rx_packets;
            else{vReleaseNetworkBufferAndDescriptor(buffer);++n->rx_dropped;}
        }else ++n->rx_dropped;
        if(!aic8800_dispatch_consume_data(n->receive))aic8800_network_fault(n);
        return;
    }
    if(!n->owns_current&&xQueueReceive(n->tx,&n->current,0)==pdPASS)n->owns_current=1;
    if(n->owns_current&&!n->active){
        if(n->current.generation!=n->generation){aic8800_network_fault(n);return;}
        if((TickType_t)(xTaskGetTickCount()-n->current.queued)>=n->config.queue_timeout){++n->tx_dropped;n->owns_current=0;return;}
        if(!aic8800_network_ready(n))return;
        if(!aic8800_network_address(n,NULL)&&!(n->config.dhcp&&!atomic_load(&n->address_failed)&&address_expired(n)==0&&dhcp_packet(n->current.ethernet,n->current.bytes))){
            ++n->tx_dropped;n->owns_current=0;return;
        }
        uint64_t now=t->card->host->last_now;
        if(n->config.tx_timeout>UINT64_MAX-now){aic8800_network_fault(n);return;}
        uint64_t deadline=now+n->config.tx_timeout;if(deadline>t->deadline)deadline=t->deadline;
        if(aic8800_transport_send_data(t,n->current.ethernet,n->current.bytes,deadline))n->active=1;
    }
}

/* Invoked by the existing IP-task trace before its current event is processed.
 * The marker is appended after down processing, so its receipt fences earlier
 * RX/output events too. Future timer/output callbacks see offline until resume. */
void aic8800_network_ip_event(unsigned event,void *data)
{
    struct aic8800_network *n=stack_owner;if(!n||!atomic_load(&n->offline))return;
    configASSERT(xIsCallingFromIPTask());
    unsigned epoch=atomic_load_explicit(&n->offline_request,memory_order_acquire);
    if(n->config.dhcp&&atomic_load_explicit(&n->resume_request,memory_order_acquire)==epoch&&
       atomic_load(&n->offline_ack)==epoch){
        /* Only the IP task retires the old lease and starts acquisition timing.
         * A concurrently recorded terminal failure must survive this request. */
        if(atomic_load(&n->failed)||atomic_load(&n->address_failed))return;
        atomic_fetch_add(&n->address_sequence,1);
        n->leased_address=0;atomic_store(&n->address_started,xTaskGetTickCount());
        atomic_store(&n->lease_started,0);atomic_store(&n->lease_ticks,0);
        atomic_store(&n->address_ready,0);atomic_store(&n->address_value,0);
        atomic_store(&n->mask_value,0);atomic_store(&n->gateway_value,0);atomic_store(&n->dns_value,0);
        memset(&n->endpoint.ipv4_settings,0,sizeof(n->endpoint.ipv4_settings));
        n->endpoint.xDHCPData.ulLeaseTime=0;
        atomic_fetch_add(&n->address_sequence,1);
        atomic_store(&n->ready,1);atomic_store_explicit(&n->offline,0,memory_order_release);
        /* The normal stack event performs vDHCPProcess(pdTRUE), including
         * fresh transaction ID, socket creation and offer/server reset. */
        FreeRTOS_NetworkDown(&n->interface);return;
    }
    if(n->offline_queued==epoch&&event==eNoEvent&&data==&n->offline_marker){
        atomic_store(&n->address_ready,0);
        atomic_store_explicit(&n->offline_ack,epoch,memory_order_release);return;
    }
    if(n->offline_queued==epoch)return;
    n->interface.bits.bInterfaceUp=pdFALSE;
    prvProcessNetworkDownEvent(&n->interface);
    if(n->config.dhcp){
        /* Down processing stopped the timer and closed its socket. Park both
         * states before the fence so an already queued DHCP event is inert. */
        n->endpoint.xDHCPData.eDHCPState=eNotUsingLeasedAddress;
        n->endpoint.xDHCPData.eExpectedState=eNotUsingLeasedAddress;
    }
    IPStackEvent_t marker={eNoEvent,&n->offline_marker};
    if(xSendEventStructToIPTask(&marker,0)==pdPASS)n->offline_queued=epoch;
}
int aic8800_network_offline(struct aic8800_network *n)
{
    if(!n||n!=stack_owner||!aic8800_network_address(n,NULL)||
       atomic_load(&n->offline)||atomic_load(&n->address_failed)||
       atomic_load(&n->offline_request)==UINT_MAX||n->active)return 0;
    atomic_store(&n->ready,0);atomic_fetch_add(&n->offline_request,1);
    atomic_store_explicit(&n->offline,1,memory_order_release);atomic_store(&n->address_ready,0);
    IPStackEvent_t wake={eNoEvent,NULL};(void)xSendEventStructToIPTask(&wake,0);return 1;
}
int aic8800_network_offline_ready(struct aic8800_network *n)
{
    if(!n||!atomic_load(&n->offline)||atomic_load(&n->failed)||
       atomic_load_explicit(&n->offline_ack,memory_order_acquire)!=atomic_load(&n->offline_request))return 0;
    if(n->active)return 0;
    if(n->owns_current){n->owns_current=0;++n->tx_dropped;}
    struct aic8800_network_packet packet;
    while(xQueueReceive(n->tx,&packet,0)==pdPASS)++n->tx_dropped;
    return 1;
}
int aic8800_network_resume(struct aic8800_network *n)
{
    if(!aic8800_network_offline_ready(n)||atomic_load(&n->address_failed)||
       !authority_for(n->config.transport,n->config.transport->wifi->association_generation)||
       !aic8800_transport_attachment_ready(n->config.transport))return 0;
    /* No IP callback can still hold an old ready=true observation across the
     * acknowledged FIFO fence. No new callback reads generation while offline. */
    n->generation=n->config.transport->wifi->association_generation;
    if(n->config.dhcp){
        unsigned epoch=atomic_load(&n->offline_request);
        if(atomic_load(&n->resume_request)!=epoch){
            atomic_store_explicit(&n->resume_request,epoch,memory_order_release);
            IPStackEvent_t wake={eNoEvent,NULL};(void)xSendEventStructToIPTask(&wake,0);
        }
        return 1;
    }
    atomic_store(&n->ready,1);atomic_store_explicit(&n->offline,0,memory_order_release);
    FreeRTOS_NetworkDown(&n->interface);return 1;
}
