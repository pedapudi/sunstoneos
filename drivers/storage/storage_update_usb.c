#include "a733_storage_update.h"
#include <string.h>
void a733_storage_channel_init(struct a733_storage_channel *c)
{*c=(struct a733_storage_channel){0};atomic_init(&c->state,0);atomic_init(&c->lost,0);}
void a733_storage_channel_lost(struct a733_storage_channel *c)
{atomic_store_explicit(&c->lost,1,memory_order_release);}
int a733_storage_channel_submit(struct a733_storage_channel *c,const uint8_t *p,size_t n)
{
    if(!c||!p||n<88||n>A733_STORAGE_BODY||atomic_load_explicit(&c->lost,memory_order_acquire))return 0;
    unsigned expected=0;
    if(!atomic_compare_exchange_strong_explicit(&c->state,&expected,4,memory_order_acquire,memory_order_relaxed)) {
        a733_storage_channel_lost(c);return 0;
    }
    memcpy(c->request,p,n);c->request_size=(unsigned)n;
    atomic_store_explicit(&c->state,1,memory_order_release);return 1;
}
void a733_storage_channel_worker(struct a733_storage_channel *c,struct a733_storage_update *s)
{
    if(!s->transport_lost)s->transport_lost=&c->lost;
    if(s->transport_lost!=&c->lost||atomic_load_explicit(&c->lost,memory_order_acquire))a733_storage_update_lost(s);
    unsigned state=atomic_load_explicit(&c->state,memory_order_acquire);
    if(state==1) {
        atomic_store_explicit(&c->state,2,memory_order_release);
        if(!a733_storage_update_request(s,c->request,c->request_size))a733_storage_channel_lost(c);
    }
    a733_storage_update_poll(s);
    if(s->state==A733_STORAGE_RETAINED){a733_storage_channel_lost(c);return;}
    if(atomic_load_explicit(&c->state,memory_order_acquire)==2&&s->response_ready) {
        size_t size;
        if(!a733_storage_update_response(s,c->response,sizeof(c->response),&size)){a733_storage_channel_lost(c);return;}
        c->response_size=(unsigned)size;atomic_store_explicit(&c->state,3,memory_order_release);
    }
}
int a733_storage_channel_response(struct a733_storage_channel *c,uint8_t *out,size_t capacity,size_t *size)
{
    if(!c||!out||!size||atomic_load_explicit(&c->lost,memory_order_acquire)||
       atomic_load_explicit(&c->state,memory_order_acquire)!=3||capacity<c->response_size)return 0;
    memcpy(out,c->response,c->response_size);*size=c->response_size;
    atomic_store_explicit(&c->state,0,memory_order_release);return 1;
}
