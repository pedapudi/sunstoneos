/* Socket event setup follows the MIT FreeRTOS+TCP socket implementation.
 * Copyright (C) 2022 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to
 * do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE. */
#include "sunstone_net.h"
#include "FreeRTOS_IP_Private.h"
#include "event_groups.h"
#include <string.h>
static struct sunstone_net *service;
static int independent(const struct sunstone_net *s,const void *p,size_t n)
{
    uintptr_t a=(uintptr_t)p,b=(uintptr_t)s;
    return p&&n<=UINTPTR_MAX-a&&sizeof(*s)<=UINTPTR_MAX-b&&!(a<b+sizeof(*s)&&b<a+n)&&
        (!s->initialized||aic8800_network_storage_valid(p,n,&s->config.network));
}
static int client(struct sunstone_net *s)
{TaskHandle_t t=xTaskGetCurrentTaskHandle();for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i)if(s->clients[i].task==t)return (int)i;return -1;}
void sunstone_net_stack_free(void *pointer)
{
    struct sunstone_net_socket *owned=NULL;
    if(service)for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i){
        uintptr_t expected=(uintptr_t)pointer;
        if(atomic_compare_exchange_strong(&service->sockets[i].disposal_address,&expected,0)){owned=&service->sockets[i];break;}
    }
    vPortFree(pointer);
    if(owned)atomic_store(&owned->disposed,1);
}
static void publish_reply(struct sunstone_net *s,struct sunstone_net_pending *p,struct sunstone_net_reply *r)
{
    if(r->result==SUNSTONE_NET_OK&&(TickType_t)(xTaskGetTickCount()-p->message.submitted)>=p->message.request.timeout){
        r->result=SUNSTONE_NET_TIMEOUT;
        if(p->slot<SUNSTONE_NET_SOCKETS&&s->sockets[p->slot].socket&&
            p->message.request.operation!=SUNSTONE_NET_SEND&&p->message.request.operation!=SUNSTONE_NET_RECEIVE)
            s->sockets[p->slot].uncertain=1;
    }
    if(xQueueSend(s->clients[p->message.client].reply_queue,r,0)!=pdPASS){atomic_store(&s->failed,1);aic8800_network_fault(&s->network);return;}
    p->active=0;
}
static void reply(struct sunstone_net *s,struct sunstone_net_pending *p,enum sunstone_net_result result,int code,const void *bytes,unsigned length,const struct freertos_sockaddr *peer)
{
    struct sunstone_net_reply r={.ticket=p->message.ticket,.handle=p->message.request.handle,.result=result,.stack_result=code,.bytes=(uint16_t)length};
    if(bytes&&length)memcpy(r.payload,bytes,length);
    if(peer){memcpy(r.address,&peer->sin_address.ulIP_IPv4,4);r.port=FreeRTOS_ntohs(peer->sin_port);}
    publish_reply(s,p,&r);
}
static struct freertos_sockaddr address(const uint8_t ip[4],uint16_t port)
{
    struct freertos_sockaddr a={.sin_len=sizeof(a),.sin_family=FREERTOS_AF_INET,.sin_port=FreeRTOS_htons(port)};
    memcpy(&a.sin_address.ulIP_IPv4,ip,4);return a;
}
static int bind_start(struct sunstone_net_socket *record,const uint8_t ip[4],uint16_t port)
{
    FreeRTOS_Socket_t *socket=record->socket;uint32_t value;memcpy(&value,ip,4);
    socket->xLocalAddress.ulIP_IPv4=FreeRTOS_ntohl(value);socket->bits.bIsIPv6=0;socket->usLocalPort=port;
    IPStackEvent_t event={eSocketBindEvent,socket};
    return xSendEventStructToIPTask(&event,0)==pdPASS;
}
static void operation(struct sunstone_net *s,struct sunstone_net_pending *p)
{
    if(!p->active)return;
    struct sunstone_net_request *r=&p->message.request;
    if((TickType_t)(xTaskGetTickCount()-p->message.submitted)>=r->timeout){
        if(p->started&&p->slot<SUNSTONE_NET_SOCKETS&&r->operation!=SUNSTONE_NET_RECEIVE&&r->operation!=SUNSTONE_NET_SEND)
            s->sockets[p->slot].uncertain=1;
        reply(s,p,SUNSTONE_NET_TIMEOUT,0,NULL,0,NULL);return;
    }
    if(r->operation==SUNSTONE_NET_EVENT){
        struct sunstone_net_reply result={.ticket=p->message.ticket,.result=SUNSTONE_NET_OK};
        if(s->network.receive&&aic8800_dispatch_take_event(s->network.receive,&result.event))publish_reply(s,p,&result);
        else if(atomic_load(&s->failed)||atomic_load(&s->network.failed))reply(s,p,SUNSTONE_NET_LINK_LOST,0,NULL,0,NULL);
        return;
    }
    if(r->operation!=SUNSTONE_NET_CLOSE&&!aic8800_network_ready(&s->network)){
        if(atomic_load(&s->failed)||atomic_load(&s->network.failed)){reply(s,p,SUNSTONE_NET_LINK_LOST,0,NULL,0,NULL);return;}
        return;
    }
    if(!p->started){
        p->slot=SUNSTONE_NET_SOCKETS;
        if(r->operation==SUNSTONE_NET_OPEN_TCP||r->operation==SUNSTONE_NET_OPEN_UDP){
            if(xIPIsNetworkTaskReady()==pdFALSE)return;
            for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)if(!s->sockets[i].socket){p->slot=i;break;}
            if(p->slot==SUNSTONE_NET_SOCKETS){reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
            if(s->next_handle==UINT64_MAX){reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;}
            struct sunstone_net_socket *record=&s->sockets[p->slot];
            record->socket=FreeRTOS_socket(FREERTOS_AF_INET,r->operation==SUNSTONE_NET_OPEN_TCP?FREERTOS_SOCK_STREAM:FREERTOS_SOCK_DGRAM,0);
            if(record->socket==FREERTOS_INVALID_SOCKET){record->socket=NULL;reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
            atomic_store(&record->disposal_address,(uintptr_t)record->socket);
            record->client=p->message.client;record->tcp=r->operation==SUNSTONE_NET_OPEN_TCP;record->handle=++s->next_handle;
            r->handle=record->handle;p->started=1;
            /* Socket defaults have zero send/receive waits in the private profile. */
            if(!bind_start(record,s->config.network.address,r->port))return;
            p->started=2;return;
        }
        for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)if(s->sockets[i].socket&&s->sockets[i].handle==r->handle&&
            s->sockets[i].client==p->message.client){p->slot=i;break;}
        if(p->slot==SUNSTONE_NET_SOCKETS){reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;}
        if(r->operation!=SUNSTONE_NET_CLOSE&&(s->sockets[p->slot].uncertain||s->sockets[p->slot].closing)){reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
        p->started=s->sockets[p->slot].closing?2:1;
    }
    struct sunstone_net_socket *record=&s->sockets[p->slot];
    if(r->operation==SUNSTONE_NET_OPEN_TCP||r->operation==SUNSTONE_NET_OPEN_UDP){
        if(p->started==1){if(bind_start(record,s->config.network.address,r->port))p->started=2;return;}
        FreeRTOS_Socket_t *socket=record->socket;
        if(!(xEventGroupGetBits(socket->xEventGroup)&eSOCKET_BOUND))return;
        if(listLIST_ITEM_CONTAINER(&socket->xBoundSocketListItem)==NULL){record->uncertain=1;reply(s,p,SUNSTONE_NET_IO,0,NULL,0,NULL);return;}
        record->bound=1;reply(s,p,SUNSTONE_NET_OK,0,NULL,0,NULL);return;
    }
    int result=0;struct freertos_sockaddr peer=address(r->address,r->port);
    switch(r->operation){
    case SUNSTONE_NET_CONNECT:
        if(!record->tcp){reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;}
        if(p->started==1){result=FreeRTOS_connect(record->socket,&peer,sizeof(peer));p->started=2;
            if(result&&result!=-pdFREERTOS_ERRNO_EWOULDBLOCK){record->uncertain=1;reply(s,p,SUNSTONE_NET_IO,result,NULL,0,NULL);return;}}
        result=FreeRTOS_issocketconnected(record->socket);if(!result)return;
        reply(s,p,result>0?SUNSTONE_NET_OK:SUNSTONE_NET_IO,result,NULL,0,NULL);return;
    case SUNSTONE_NET_SEND:
        result=record->tcp?FreeRTOS_send(record->socket,r->payload,r->bytes,0):FreeRTOS_sendto(record->socket,r->payload,r->bytes,0,&peer,sizeof(peer));
        if(!result||result==-pdFREERTOS_ERRNO_EWOULDBLOCK)return;
        reply(s,p,result>0?SUNSTONE_NET_OK:SUNSTONE_NET_IO,result,NULL,0,NULL);return;
    case SUNSTONE_NET_RECEIVE:{
        uint8_t bytes[SUNSTONE_NET_BYTES];socklen_t peer_bytes=sizeof(peer);
        result=record->tcp?FreeRTOS_recv(record->socket,bytes,r->bytes,0):FreeRTOS_recvfrom(record->socket,bytes,r->bytes,0,&peer,&peer_bytes);
        if(!result||result==-pdFREERTOS_ERRNO_EWOULDBLOCK)return;
        reply(s,p,result>0?SUNSTONE_NET_OK:SUNSTONE_NET_IO,result,bytes,result>0?(unsigned)result:0,record->tcp?NULL:&peer);return;}
    case SUNSTONE_NET_CLOSE:
        if(p->started==1){IPStackEvent_t event={eSocketCloseEvent,record->socket};
            if(xSendEventStructToIPTask(&event,0)!=pdPASS)return;
            record->closing=1;p->started=2;return;}
        if(!atomic_load(&record->disposed))return;
        record->socket=NULL;record->handle=0;record->client=0;
        record->tcp=record->bound=record->closing=record->uncertain=0;
        atomic_store(&record->disposal_address,0);atomic_store(&record->disposed,0);
        reply(s,p,SUNSTONE_NET_OK,0,NULL,0,NULL);return;
    default:reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;
    }
}
static void worker(void *context)
{
    struct sunstone_net *s=context;
    while(!atomic_load(&s->activation))vTaskDelay(1);
    if(!aic8800_network_activate(&s->network,&s->config.network))atomic_store(&s->failed,1);
    for(;;){
        if(atomic_load(&s->failed))aic8800_network_fault(&s->network);
        aic8800_network_poll(&s->network);
        struct sunstone_net_message message;
        if(xQueueReceive(s->requests,&message,0)==pdPASS){
            struct sunstone_net_pending *p=&s->pending[message.client];configASSERT(!p->active);
            *p=(struct sunstone_net_pending){.message=message,.active=1,.slot=SUNSTONE_NET_SOCKETS};
        }
        for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i)operation(s,&s->pending[i]);
        vTaskDelay(1);
    }
}
int sunstone_net_init(struct sunstone_net *s,const struct sunstone_net_config *c)
{
    if(!s||!c||service||s->initialized||!independent(s,c,sizeof(*c))||!c->bootstrap||
        !aic8800_network_storage_valid(s,sizeof(*s),&c->network)||
        xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i)if(c->clients[i])for(unsigned j=0;j<i;++j)if(c->clients[j]==c->clients[i])return 0;
    memset(s,0,sizeof(*s));s->initialized=1;s->config=*c;service=s;
    s->requests=xQueueCreateStatic(SUNSTONE_NET_CLIENTS,sizeof(struct sunstone_net_message),s->request_storage,&s->request_queue);
    for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i){s->clients[i].task=c->clients[i];s->clients[i].reply_queue=xQueueCreateStatic(1,sizeof(struct sunstone_net_reply),s->clients[i].storage,&s->clients[i].queue);}
    s->worker=xTaskCreateStatic(worker,"network",SUNSTONE_NET_STACK_WORDS,s,4,s->stack,&s->task);
    return s->requests&&s->worker;
}
int sunstone_net_activate(struct sunstone_net *s)
{
    if(!s||!s->initialized||xTaskGetCurrentTaskHandle()!=s->config.bootstrap||atomic_load(&s->activation))return 0;
    atomic_store(&s->activation,1);return 1;
}
int sunstone_net_submit(struct sunstone_net *s,const struct sunstone_net_request *r,uint64_t *ticket)
{
    if(!s||!s->initialized||!independent(s,r,sizeof(*r))||!independent(s,ticket,sizeof(*ticket))||
        !r->timeout||r->timeout>(TickType_t)(portMAX_DELAY/2)||r->operation>SUNSTONE_NET_EVENT||r->bytes>SUNSTONE_NET_BYTES||
        ((r->operation==SUNSTONE_NET_SEND||r->operation==SUNSTONE_NET_RECEIVE)&&!r->bytes))return 0;
    int i=client(s);if(i<0)return 0;struct sunstone_net_client *c=&s->clients[i];
    if(c->pending||c->next_ticket==UINT64_MAX)return 0;
    struct sunstone_net_message m={.request=*r,.client=(unsigned)i,.ticket=++c->next_ticket,.submitted=xTaskGetTickCount()};
    if(xQueueSend(s->requests,&m,0)!=pdPASS)return 0;
    c->pending=m.ticket;*ticket=m.ticket;return 1;
}
int sunstone_net_receive(struct sunstone_net *s,struct sunstone_net_reply *reply,TickType_t wait)
{
    if(!s||!s->initialized||!independent(s,reply,sizeof(*reply))||wait>(TickType_t)(portMAX_DELAY/2))return 0;
    int i=client(s);if(i<0||!s->clients[i].pending)return 0;
    if(xQueueReceive(s->clients[i].reply_queue,reply,wait)!=pdPASS)return 0;
    if(reply->ticket!=s->clients[i].pending){atomic_store(&s->failed,1);return 0;}
    s->clients[i].pending=0;return 1;
}
