#include "aic8800_network.h"
#include "aic8800_data.h"
#include "aic8800_security.h"
#include "aic8800_supplicant.h"
#include "aic8800_runtime.h"
#include "aic8800_firmware.h"
#include "FreeRTOS_IP_Private.h"
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
    return disjoint(p,bytes,t,sizeof(*t))&&t->card&&disjoint(p,bytes,t->card,sizeof(*t->card))&&
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
{return n&&n->initialized&&!atomic_load(&n->failed)&&atomic_load(&n->ready);}
void aic8800_network_fault(struct aic8800_network *n)
{if(n&&n->initialized){atomic_store(&n->ready,0);atomic_store(&n->failed,1);}}
static BaseType_t link_status(NetworkInterface_t *interface)
{return aic8800_network_ready(interface->pvArgument)?pdTRUE:pdFALSE;}
static BaseType_t output(NetworkInterface_t *interface,NetworkBufferDescriptor_t *const descriptor,BaseType_t release)
{
    struct aic8800_network *n=interface->pvArgument;BaseType_t result=pdFAIL;
    if(descriptor&&descriptor->pucEthernetBuffer&&descriptor->xDataLength>=14&&descriptor->xDataLength<=1514&&aic8800_network_ready(n)) {
        const uint8_t *p=descriptor->pucEthernetBuffer;
        if(!(p[12]==0x88&&p[13]==0x8e)) {
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
    if(!n||atomic_load(&n->failed)||!value||n->config.random32(n->config.entropy_context,value)!=1){
        if(value)*value=0;aic8800_network_fault(n);return pdFALSE;}
    return pdTRUE;
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
        !c->tx_timeout||!c->queue_timeout||!c->entropy_record||!c->random32||!c->sequence)return 0;
    uint64_t generation=c->transport->wifi->association_generation;
    if(!generation||!authority_for(c->transport,generation)||
        c->transport->entered||c->transport->pending||c->transport->data_tx||c->transport->security_tx||
        c->transport->data->state!=AIC8800_DATA_IDLE||c->transport->card->state!=SDIO_CARD_READY||
        c->transport->card->host->state!=SUNXI_SDIO_READY)return 0;
    uintptr_t begin=(uintptr_t)n,end=(uintptr_t)c;
    if(sizeof(*n)>UINTPTR_MAX-begin||sizeof(*c)>UINTPTR_MAX-end||
        (begin<end+sizeof(*c)&&end<begin+sizeof(*n)))return 0;
    memset(n,0,sizeof(*n));n->initialized=1;n->config=*c;n->generation=generation;
    n->tx=xQueueCreateStatic(AIC8800_NETWORK_TX_SLOTS,sizeof(struct aic8800_network_packet),n->tx_storage,&n->tx_queue);
    if(!n->tx){aic8800_network_fault(n);return 0;}
    n->interface=(NetworkInterface_t){.pcName="wifi",.pvArgument=n,.pfInitialise=link_status,.pfOutput=output,.pfGetPhyLinkStatus=link_status};
    static const uint8_t dns[4]={0};
    FreeRTOS_AddNetworkInterface(&n->interface);
    FreeRTOS_FillEndPoint(&n->interface,&n->endpoint,c->address,c->netmask,c->gateway,dns,c->transport->wifi->mac);
    stack_owner=n;atomic_store(&n->ready,1);
    if(FreeRTOS_IPInit_Multi()!=pdPASS){aic8800_network_fault(n);return 0;}return 1;
}
void aic8800_network_poll(struct aic8800_network *n)
{
    if(!n||!n->initialized)return;
    struct aic8800_sdio_transport *t=n->config.transport;
    if(atomic_load(&n->failed)){if(!n->down_sent){FreeRTOS_NetworkDown(&n->interface);n->down_sent=1;}return;}
    aic8800_transport_poll(t);
    if(t->state==AIC8800_TRANSPORT_FAULT||!t->wifi||t->wifi->association_generation!=n->generation||
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
        if(!aic8800_transport_peek_data(t,&frame)||frame.eapol)return; /* Other dispatcher retains raw/control ownership. */
        if(frame.generation!=n->generation){aic8800_network_fault(n);return;}
        NetworkBufferDescriptor_t *buffer=aic8800_network_ready(n)?pxGetNetworkBufferWithDescriptor(frame.bytes,0):NULL;
        if(buffer){
            memcpy(buffer->pucEthernetBuffer,frame.ethernet,frame.bytes);buffer->xDataLength=frame.bytes;
            buffer->pxInterface=&n->interface;buffer->pxEndPoint=&n->endpoint;
            IPStackEvent_t event={eNetworkRxEvent,buffer};
            if(xSendEventStructToIPTask(&event,0)==pdPASS)++n->rx_packets;
            else{vReleaseNetworkBufferAndDescriptor(buffer);++n->rx_dropped;}
        }else ++n->rx_dropped;
        if(!aic8800_transport_consume(t))aic8800_network_fault(n);
        return;
    }
    if(!n->owns_current&&xQueueReceive(n->tx,&n->current,0)==pdPASS)n->owns_current=1;
    if(n->owns_current&&!n->active){
        if(n->current.generation!=n->generation){aic8800_network_fault(n);return;}
        if((TickType_t)(xTaskGetTickCount()-n->current.queued)>=n->config.queue_timeout){++n->tx_dropped;n->owns_current=0;return;}
        if(!aic8800_network_ready(n))return;
        uint64_t now=t->card->host->last_now;
        if(n->config.tx_timeout>UINT64_MAX-now){aic8800_network_fault(n);return;}
        uint64_t deadline=now+n->config.tx_timeout;if(deadline>t->deadline)deadline=t->deadline;
        if(aic8800_transport_send_data(t,n->current.ethernet,n->current.bytes,deadline))n->active=1;
    }
}
