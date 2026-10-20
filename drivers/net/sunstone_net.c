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
#include "FreeRTOS_DNS.h"
#include "event_groups.h"
#include <string.h>
static struct sunstone_net *service;
static void accept_ip_step(struct sunstone_net *s)
{
    for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i){
        struct sunstone_net_accept *a=&s->accept[i];
        unsigned state=atomic_load_explicit(&a->state,memory_order_acquire);
        if(state==SUNSTONE_ACCEPT_CANCEL){atomic_store_explicit(&a->state,SUNSTONE_ACCEPT_CANCELED,memory_order_release);continue;}
        unsigned expected=SUNSTONE_ACCEPT_START;
        if(state!=expected||!atomic_compare_exchange_strong(&a->state,&expected,SUNSTONE_ACCEPT_RUNNING))continue;
        if((TickType_t)(xTaskGetTickCount()-a->submitted)>=a->timeout||
           atomic_load(&s->failed)||atomic_load(&s->network.failed)||
           !aic8800_network_address(&s->network,NULL)){
            a->result=(TickType_t)(xTaskGetTickCount()-a->submitted)>=a->timeout?SUNSTONE_NET_TIMEOUT:
                atomic_load(&s->failed)||atomic_load(&s->network.failed)?SUNSTONE_NET_LINK_LOST:SUNSTONE_NET_ADDRESS_LOST;
            atomic_store_explicit(&a->state,SUNSTONE_ACCEPT_CANCELED,memory_order_release);continue;
        }
        Socket_t listener=s->sockets[a->listener].socket;
        if(a->operation==SUNSTONE_NET_LISTEN){
            a->result=FreeRTOS_listen(listener,1);
        }else{
            socklen_t bytes=sizeof(a->peer);
            /* The pinned accept success path queues eTCPAcceptEvent with
             * portMAX_DELAY. Its IP-task sender forces that wait to zero. */
            Socket_t child=FreeRTOS_accept(listener,&a->peer,&bytes);
            if(child==NULL){atomic_store_explicit(&a->state,SUNSTONE_ACCEPT_START,memory_order_release);continue;}
            a->result=child==FREERTOS_INVALID_SOCKET?-1:0;
            if(!a->result){
                a->socket=child;
                /* Registration precedes publication; an accepted child has
                 * left the stack's unaccepted-child automatic close set. */
                atomic_store(&s->sockets[a->child].disposal_address,(uintptr_t)child);
            }
        }
        atomic_store_explicit(&a->state,SUNSTONE_ACCEPT_DONE,memory_order_release);
    }
}
/* These two call sites are renamed only in the pinned FreeRTOS_DNS.c.
 * Every lookup and cancellation executes in the sole IP task, including its
 * normal parser/timer callbacks. No public bind/close wait is entered. */
BaseType_t sunstone_net_dns_bind(Socket_t socket,uint16_t port)
{
    configASSERT(xIsCallingFromIPTask());
    struct freertos_sockaddr a={.sin_family=FREERTOS_AF_INET,.sin_port=port};
    return vSocketBind(socket,&a,sizeof(a),pdFALSE);
}
void sunstone_net_dns_close(Socket_t socket)
{configASSERT(xIsCallingFromIPTask());if(socket&&socket!=FREERTOS_INVALID_SOCKET)(void)vSocketClose(socket);}
static void dns_result(const char *name,void *context,struct freertos_addrinfo *address)
{
    (void)name;struct sunstone_net_dns *q=context;configASSERT(xIsCallingFromIPTask());
    if(atomic_load(&q->state)==SUNSTONE_DNS_WAIT){
        q->address=address&&address->ai_family==FREERTOS_AF_INET&&address->ai_addr?
            address->ai_addr->sin_address.ulIP_IPv4:0;
        atomic_store(&q->callback_ready,1);
    }
}
void sunstone_net_ip_step(void)
{
    struct sunstone_net *s=service;if(!s)return;configASSERT(xIsCallingFromIPTask());
    aic8800_network_ip_step();
    accept_ip_step(s);
    for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i){
        struct sunstone_net_dns *q=&s->dns[i];unsigned state=atomic_load(&q->state);
        uint64_t canceled=atomic_exchange(&q->cancel_requested,0);
        if(canceled&&canceled==atomic_load(&q->ticket)&&
            (state==SUNSTONE_DNS_START||state==SUNSTONE_DNS_WAIT||state==SUNSTONE_DNS_DONE)){
            unsigned expected=state;
            if(atomic_compare_exchange_strong(&q->state,&expected,SUNSTONE_DNS_CANCEL)){
                FreeRTOS_gethostbyname_cancel(q);atomic_store(&q->state,SUNSTONE_DNS_CANCELED);
            }
            continue;
        }
        if(state==SUNSTONE_DNS_WAIT&&atomic_load(&q->callback_ready)){
            FreeRTOS_gethostbyname_cancel(q);atomic_store(&q->state,SUNSTONE_DNS_DONE);continue;
        }
        if(state!=SUNSTONE_DNS_START)continue;
        TickType_t elapsed=xTaskGetTickCount()-q->submitted;
        if(elapsed>=q->timeout||!aic8800_network_address(&s->network,NULL)){
            q->address=0;atomic_store(&q->state,SUNSTONE_DNS_DONE);continue;
        }
        /* API callback timeout is milliseconds, request timeout is ticks. The
         * owner independently enforces its earlier absolute tick deadline. */
        TickType_t remaining=q->timeout-elapsed;
        uint64_t milliseconds=remaining>(portMAX_DELAY/2)/1000?portMAX_DELAY/2:
            (uint64_t)remaining*1000/configTICK_RATE_HZ;
        if(!milliseconds)milliseconds=1;if(milliseconds>portMAX_DELAY/2)milliseconds=portMAX_DELAY/2;
        atomic_store(&q->callback_ready,0);atomic_store(&q->state,SUNSTONE_DNS_WAIT);
        uint32_t value=FreeRTOS_gethostbyname_a(q->name,dns_result,q,(TickType_t)milliseconds);
        if(value){FreeRTOS_gethostbyname_cancel(q);q->address=value;atomic_store(&q->state,SUNSTONE_DNS_DONE);}
    }
}
static void dns_cancel(struct sunstone_net *s,unsigned client)
{
    struct sunstone_net_dns *q=&s->dns[client];unsigned state=atomic_load(&q->state);
    if(state!=SUNSTONE_DNS_IDLE&&state!=SUNSTONE_DNS_CANCELED){
        atomic_store(&q->cancel_requested,atomic_load(&q->ticket));
        IPStackEvent_t wake={eNoEvent,NULL};(void)xSendEventStructToIPTask(&wake,0);
    }
}
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
    if(p->message.request.operation==SUNSTONE_NET_RESOLVE&&r->result!=SUNSTONE_NET_OK)dns_cancel(s,p->message.client);
    atomic_store(&s->cancel[p->message.client],0);
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
static enum sunstone_net_result accept_stop(struct sunstone_net *s,struct sunstone_net_pending *p)
{
    if(atomic_load(&s->cancel[p->message.client])==p->message.ticket)return SUNSTONE_NET_CANCELED;
    if((TickType_t)(xTaskGetTickCount()-p->message.submitted)>=p->message.request.timeout)return SUNSTONE_NET_TIMEOUT;
    if(atomic_load(&s->failed)||atomic_load(&s->network.failed))return SUNSTONE_NET_LINK_LOST;
    if(!aic8800_network_address(&s->network,NULL))return SUNSTONE_NET_ADDRESS_LOST;
    return SUNSTONE_NET_OK;
}
static void accept_collect(struct sunstone_net *s,struct sunstone_net_pending *p)
{
    struct sunstone_net_accept *a=&s->accept[p->message.client];
    enum sunstone_net_result stopped=accept_stop(s,p);
    unsigned state=atomic_load_explicit(&a->state,memory_order_acquire);
    if(stopped!=SUNSTONE_NET_OK&&state==SUNSTONE_ACCEPT_START){
        unsigned expected=state;
        (void)atomic_compare_exchange_strong(&a->state,&expected,SUNSTONE_ACCEPT_CANCEL);
        state=atomic_load_explicit(&a->state,memory_order_acquire);
    }
    if(state!=SUNSTONE_ACCEPT_DONE&&state!=SUNSTONE_ACCEPT_CANCELED){
        IPStackEvent_t wake={eNoEvent,NULL};(void)xSendEventStructToIPTask(&wake,0);return;
    }
    struct sunstone_net_socket *parent=&s->sockets[a->listener],*child=&s->sockets[a->child];
    enum sunstone_net_result result=state==SUNSTONE_ACCEPT_CANCELED?
        (stopped!=SUNSTONE_NET_OK?stopped:(enum sunstone_net_result)a->result):
        a->result?SUNSTONE_NET_IO:SUNSTONE_NET_OK;
    if(stopped!=SUNSTONE_NET_OK)result=stopped;
    const struct freertos_sockaddr *peer=NULL;
    if(a->operation==SUNSTONE_NET_LISTEN){
        if(state!=SUNSTONE_ACCEPT_DONE||a->result){parent->listening=0;child->parent=0;}
        else if(result!=SUNSTONE_NET_OK)parent->uncertain=1;
    }else if(a->socket){
        child->socket=a->socket;child->tcp=child->bound=1;parent->accepted=1;
        if(result!=SUNSTONE_NET_OK)child->uncertain=1;
        p->message.request.handle=child->handle;p->slot=a->child;peer=&a->peer;
    }else{child->handle=0;child->client=0;}
    atomic_store_explicit(&a->state,SUNSTONE_ACCEPT_IDLE,memory_order_release);
    reply(s,p,result,a->result,NULL,0,peer);
}
static void operation(struct sunstone_net *s,struct sunstone_net_pending *p)
{
    if(!p->active)return;
    struct sunstone_net_request *r=&p->message.request;
    /* An IP-task handoff must finish before cancellation, link/address loss,
     * timeout or CLOSE can abandon its listener or accepted child identity. */
    if((r->operation==SUNSTONE_NET_LISTEN||r->operation==SUNSTONE_NET_ACCEPT)&&p->started==3){accept_collect(s,p);return;}
    if(atomic_load(&s->cancel[p->message.client])==p->message.ticket){
        if(p->started&&p->slot<SUNSTONE_NET_SOCKETS&&r->operation!=SUNSTONE_NET_RECEIVE&&r->operation!=SUNSTONE_NET_SEND)
            s->sockets[p->slot].uncertain=1;
        reply(s,p,SUNSTONE_NET_CANCELED,0,NULL,0,NULL);return;
    }
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
    if(r->operation==SUNSTONE_NET_ADDRESS){
        if(!aic8800_network_address(&s->network,NULL)){
            if(atomic_load(&s->network.address_failed))reply(s,p,SUNSTONE_NET_ADDRESS_LOST,0,NULL,0,NULL);
            return;
        }
        struct sunstone_net_reply result={.ticket=p->message.ticket,.result=SUNSTONE_NET_OK,.bytes=16};
        uint32_t values[4];if(!aic8800_network_status(&s->network,values))return;
        memcpy(result.address,values,4);memcpy(result.payload,values,sizeof(values));publish_reply(s,p,&result);return;
    }
    if(r->operation!=SUNSTONE_NET_CLOSE&&!aic8800_network_address(&s->network,NULL)){
        if(atomic_load(&s->network.address_failed))reply(s,p,SUNSTONE_NET_ADDRESS_LOST,0,NULL,0,NULL);
        return;
    }
    if(r->operation==SUNSTONE_NET_RESOLVE){
        struct sunstone_net_dns *q=&s->dns[p->message.client];unsigned state=atomic_load(&q->state);
        if(!p->started){
            if(!atomic_load(&s->network.dns_value)){reply(s,p,SUNSTONE_NET_IO,0,NULL,0,NULL);return;}
            if(state==SUNSTONE_DNS_CANCELED){atomic_store(&q->state,SUNSTONE_DNS_IDLE);state=SUNSTONE_DNS_IDLE;}
            if(state!=SUNSTONE_DNS_IDLE){reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
            memcpy(q->name,r->payload,r->bytes);q->name[r->bytes]=0;q->ticket=p->message.ticket;
            q->submitted=p->message.submitted;q->timeout=r->timeout;q->address=0;
            atomic_store(&q->state,SUNSTONE_DNS_START);p->started=1;
        }else if(state==SUNSTONE_DNS_DONE){
            unsigned expected=SUNSTONE_DNS_DONE;
            if(!atomic_compare_exchange_strong(&q->state,&expected,SUNSTONE_DNS_IDLE))return;
            uint32_t ip=q->address;
            /* DONE is published only after the IP task's final access. */
            struct sunstone_net_reply result={.ticket=p->message.ticket,.result=ip?SUNSTONE_NET_OK:SUNSTONE_NET_IO};
            memcpy(result.address,&ip,4);publish_reply(s,p,&result);return;
        }
        IPStackEvent_t wake={eNoEvent,NULL};(void)xSendEventStructToIPTask(&wake,0);return;
    }
    if(!p->started){
        p->slot=SUNSTONE_NET_SOCKETS;
        if(r->operation==SUNSTONE_NET_OPEN_TCP||r->operation==SUNSTONE_NET_OPEN_UDP){
            if(xIPIsNetworkTaskReady()==pdFALSE)return;
            for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)if(!s->sockets[i].socket&&!s->sockets[i].parent){p->slot=i;break;}
            if(p->slot==SUNSTONE_NET_SOCKETS){reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
            if(s->next_handle==UINT64_MAX){reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;}
            struct sunstone_net_socket *record=&s->sockets[p->slot];
            record->socket=FreeRTOS_socket(FREERTOS_AF_INET,r->operation==SUNSTONE_NET_OPEN_TCP?FREERTOS_SOCK_STREAM:FREERTOS_SOCK_DGRAM,0);
            if(record->socket==FREERTOS_INVALID_SOCKET){record->socket=NULL;reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
            atomic_store(&record->disposal_address,(uintptr_t)record->socket);
            record->client=p->message.client;record->tcp=r->operation==SUNSTONE_NET_OPEN_TCP;record->handle=++s->next_handle;
            r->handle=record->handle;p->started=1;
            /* Socket defaults have zero send/receive waits in the private profile. */
            if(!bind_start(record,(const uint8_t *)&(uint32_t){atomic_load(&s->network.address_value)},r->port))return;
            p->started=2;return;
        }
        for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)if(s->sockets[i].socket&&s->sockets[i].handle==r->handle&&
            s->sockets[i].client==p->message.client){p->slot=i;break;}
        if(p->slot==SUNSTONE_NET_SOCKETS){reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;}
        if(r->operation!=SUNSTONE_NET_CLOSE&&(s->sockets[p->slot].uncertain||s->sockets[p->slot].closing)){reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
        p->started=s->sockets[p->slot].closing?2:1;
    }
    struct sunstone_net_socket *record=&s->sockets[p->slot];
    if(record->listening&&r->operation!=SUNSTONE_NET_LISTEN&&r->operation!=SUNSTONE_NET_ACCEPT&&r->operation!=SUNSTONE_NET_CLOSE){reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;}
    if(r->operation==SUNSTONE_NET_OPEN_TCP||r->operation==SUNSTONE_NET_OPEN_UDP){
        if(p->started==1){if(bind_start(record,(const uint8_t *)&(uint32_t){atomic_load(&s->network.address_value)},r->port))p->started=2;return;}
        FreeRTOS_Socket_t *socket=record->socket;
        if(!(xEventGroupGetBits(socket->xEventGroup)&eSOCKET_BOUND))return;
        if(listLIST_ITEM_CONTAINER(&socket->xBoundSocketListItem)==NULL){record->uncertain=1;reply(s,p,SUNSTONE_NET_IO,0,NULL,0,NULL);return;}
        record->bound=1;reply(s,p,SUNSTONE_NET_OK,0,NULL,0,NULL);return;
    }
    int result=0;struct freertos_sockaddr peer=address(r->address,r->port);
    switch(r->operation){
    case SUNSTONE_NET_LISTEN:
    case SUNSTONE_NET_ACCEPT:{
        if(!record->tcp||!record->bound){reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;}
        unsigned child=SUNSTONE_NET_SOCKETS;
        if(r->operation==SUNSTONE_NET_LISTEN){
            for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)if(s->sockets[i].listening){reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
            for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)if(!s->sockets[i].socket&&!s->sockets[i].parent){child=i;break;}
            if(child==SUNSTONE_NET_SOCKETS){reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
            record->listening=1;s->sockets[child].parent=p->slot+1;
        }else{
            if(!record->listening||record->accepted||s->next_handle==UINT64_MAX){reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;}
            for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)if(s->sockets[i].parent==p->slot+1){child=i;break;}
            if(child==SUNSTONE_NET_SOCKETS||s->sockets[child].socket){reply(s,p,SUNSTONE_NET_BUSY,0,NULL,0,NULL);return;}
            s->sockets[child].handle=++s->next_handle;s->sockets[child].client=p->message.client;
        }
        struct sunstone_net_accept *a=&s->accept[p->message.client];
        configASSERT(atomic_load(&a->state)==SUNSTONE_ACCEPT_IDLE);
        a->operation=r->operation;a->listener=p->slot;a->child=child;
        a->submitted=p->message.submitted;a->timeout=r->timeout;a->socket=NULL;a->result=0;
        memset(&a->peer,0,sizeof(a->peer));
        atomic_store_explicit(&a->state,SUNSTONE_ACCEPT_START,memory_order_release);p->started=3;
        IPStackEvent_t wake={eNoEvent,NULL};(void)xSendEventStructToIPTask(&wake,0);return;
    }
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
        if(record->listening)for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)
            if(s->sockets[i].parent==p->slot+1)s->sockets[i].parent=0;
        record->socket=NULL;record->handle=0;record->client=0;
        record->tcp=record->bound=record->closing=record->uncertain=0;
        record->listening=record->accepted=0;
        atomic_store(&record->disposal_address,0);atomic_store(&record->disposed,0);
        reply(s,p,SUNSTONE_NET_OK,0,NULL,0,NULL);return;
    default:reply(s,p,SUNSTONE_NET_INVALID,0,NULL,0,NULL);return;
    }
}
static void service_step(struct sunstone_net *s)
{
    if(atomic_load(&s->failed))aic8800_network_fault(&s->network);
    aic8800_network_poll(&s->network);
    struct sunstone_net_message message;
    if(xQueueReceive(s->requests,&message,0)==pdPASS){
        struct sunstone_net_pending *p=&s->pending[message.client];configASSERT(!p->active);
        *p=(struct sunstone_net_pending){.message=message,.active=1,.slot=SUNSTONE_NET_SOCKETS};
    }
    for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i)operation(s,&s->pending[i]);
}
static void worker(void *context)
{
    struct sunstone_net *s=context;
    while(!atomic_load(&s->activation))vTaskDelay(1);
    if(!aic8800_network_activate(&s->network,&s->config.network))atomic_store(&s->failed,1);
    for(;;){service_step(s);vTaskDelay(1);}
}
static int initialize(struct sunstone_net *s,const struct sunstone_net_config *c,unsigned owned)
{
    if(!s||!c||service||s->initialized||!independent(s,c,sizeof(*c))||!c->bootstrap||
        !aic8800_network_storage_valid(s,sizeof(*s),&c->network)||
        (owned ? (xTaskGetSchedulerState()!=taskSCHEDULER_RUNNING || xTaskGetCurrentTaskHandle()!=c->bootstrap) :
         xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED))return 0;
    for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i)if(c->clients[i])for(unsigned j=0;j<i;++j)if(c->clients[j]==c->clients[i])return 0;
    memset(s,0,sizeof(*s));s->initialized=1;s->owned_poll=(uint8_t)owned;s->config=*c;service=s;
    s->requests=xQueueCreateStatic(SUNSTONE_NET_CLIENTS,sizeof(struct sunstone_net_message),s->request_storage,&s->request_queue);
    for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i){s->clients[i].task=c->clients[i];s->clients[i].reply_queue=xQueueCreateStatic(1,sizeof(struct sunstone_net_reply),s->clients[i].storage,&s->clients[i].queue);}
    s->worker=owned ? c->bootstrap : xTaskCreateStatic(worker,"network",SUNSTONE_NET_STACK_WORDS,s,4,s->stack,&s->task);
    return s->requests&&s->worker;
}
int sunstone_net_init(struct sunstone_net *s,const struct sunstone_net_config *c)
{return initialize(s,c,0);}
int sunstone_net_init_owned(struct sunstone_net *s,const struct sunstone_net_config *c)
{return initialize(s,c,1);}
int sunstone_net_poll_owned(struct sunstone_net *s)
{
    if(!s || !s->initialized || !s->owned_poll || !atomic_load(&s->activation) ||
        xTaskGetCurrentTaskHandle()!=s->worker)return 0;
    service_step(s);return 1;
}
int sunstone_net_activate(struct sunstone_net *s)
{
    if(!s||!s->initialized||xTaskGetCurrentTaskHandle()!=s->config.bootstrap||atomic_load(&s->activation))return 0;
    if(s->owned_poll && !aic8800_network_activate(&s->network,&s->config.network)){
        atomic_store(&s->failed,1);aic8800_network_fault(&s->network);
        atomic_store(&s->activation,1);return 0;
    }
    atomic_store(&s->activation,1);return 1;
}
int sunstone_net_submit(struct sunstone_net *s,const struct sunstone_net_request *r,uint64_t *ticket)
{
    if(!s||!s->initialized||!independent(s,r,sizeof(*r))||!independent(s,ticket,sizeof(*ticket))||
        !r->timeout||r->timeout>(TickType_t)(portMAX_DELAY/2)||r->operation>SUNSTONE_NET_ACCEPT||r->bytes>SUNSTONE_NET_BYTES||
        ((r->operation==SUNSTONE_NET_SEND||r->operation==SUNSTONE_NET_RECEIVE)&&!r->bytes))return 0;
    if((r->operation==SUNSTONE_NET_LISTEN||r->operation==SUNSTONE_NET_ACCEPT)&&
       (!r->handle||r->bytes||r->port||r->address[0]||r->address[1]||r->address[2]||r->address[3]))return 0;
    if(r->operation==SUNSTONE_NET_RESOLVE){
        if(!r->bytes||r->bytes>=SUNSTONE_NET_NAME_BYTES||r->handle||r->port)return 0;
        unsigned label=0;
        for(unsigned j=0;j<r->bytes;++j){unsigned ch=r->payload[j];
            if(ch=='.'){if(!label)return 0;label=0;continue;}
            if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-')||++label>63)return 0;}
        if(!label)return 0;
    }
    int i=client(s);if(i<0)return 0;struct sunstone_net_client *c=&s->clients[i];
    unsigned unlocked=0;
    if(!atomic_compare_exchange_strong(&s->client_gate,&unlocked,1))return 0;
    if(atomic_load(&s->offline)||c->pending||c->next_ticket==UINT64_MAX){atomic_store(&s->client_gate,0);return 0;}
    struct sunstone_net_message m={.request=*r,.client=(unsigned)i,.ticket=++c->next_ticket,.submitted=xTaskGetTickCount()};
    if(xQueueSend(s->requests,&m,0)!=pdPASS){atomic_store(&s->client_gate,0);return 0;}
    c->pending=m.ticket;*ticket=m.ticket;atomic_store(&s->client_gate,0);return 1;
}
int sunstone_net_receive(struct sunstone_net *s,struct sunstone_net_reply *reply,TickType_t wait)
{
    if(!s||!s->initialized||!independent(s,reply,sizeof(*reply))||wait>(TickType_t)(portMAX_DELAY/2))return 0;
    int i=client(s);if(i<0||!s->clients[i].pending)return 0;
    if(xQueueReceive(s->clients[i].reply_queue,reply,wait)!=pdPASS)return 0;
    if(reply->ticket!=s->clients[i].pending){atomic_store(&s->failed,1);return 0;}
    atomic_store_explicit(&s->clients[i].pending,0,memory_order_release);return 1;
}

int sunstone_net_cancel(struct sunstone_net *s,uint64_t ticket)
{
    if(!s||!s->initialized||!ticket)return 0;int i=client(s);
    if(i<0||s->clients[i].pending!=ticket)return 0;
    atomic_store(&s->cancel[i],ticket);return 1;
}

void sunstone_net_ip_event(unsigned event,void *data)
{
    sunstone_net_ip_step();aic8800_network_ip_event(event,data);
}
int sunstone_net_offline(struct sunstone_net *s)
{
    if(!s||!s->initialized||!s->owned_poll||xTaskGetCurrentTaskHandle()!=s->config.bootstrap||
       atomic_load(&s->failed)||atomic_load(&s->offline))return 0;
    unsigned unlocked=0;if(!atomic_compare_exchange_strong(&s->client_gate,&unlocked,1))return 0;
    int ok=uxQueueMessagesWaiting(s->requests)==0;
    for(unsigned i=0;i<SUNSTONE_NET_CLIENTS;++i)
        if(s->clients[i].pending||s->pending[i].active||uxQueueMessagesWaiting(s->clients[i].reply_queue)||
           atomic_load(&s->accept[i].state)!=SUNSTONE_ACCEPT_IDLE||
           (atomic_load(&s->dns[i].state)!=SUNSTONE_DNS_IDLE&&atomic_load(&s->dns[i].state)!=SUNSTONE_DNS_CANCELED))ok=0;
    for(unsigned i=0;i<SUNSTONE_NET_SOCKETS;++i)
        if(s->sockets[i].socket||s->sockets[i].handle||s->sockets[i].parent||atomic_load(&s->sockets[i].disposal_address))ok=0;
    if(ok){atomic_store(&s->offline,1);ok=aic8800_network_offline(&s->network);if(!ok)atomic_store(&s->offline,0);}
    atomic_store(&s->client_gate,0);return ok;
}
int sunstone_net_resume(struct sunstone_net *s)
{
    if(!s||!s->owned_poll||xTaskGetCurrentTaskHandle()!=s->config.bootstrap||
       !atomic_load(&s->offline)||!aic8800_network_address(&s->network,NULL))return 0;
    atomic_store_explicit(&s->offline,0,memory_order_release);return 1;
}
