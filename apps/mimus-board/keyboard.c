#include "keyboard.h"
#include <stdatomic.h>
#include <string.h>
static atomic_uint full,accepted,fault;
static struct mimus_keyboard_report mailbox,baseline,previous;
static unsigned neutral;
static uint8_t evidence[128];
static int fail(void){atomic_store_explicit(&fault,1,memory_order_release);return -1;}
static int consumed(int r){atomic_store_explicit(&full,0,memory_order_release);return r;}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
int mimus_keyboard_empty(void)
{return !atomic_load_explicit(&full,memory_order_acquire)&&!atomic_load_explicit(&accepted,memory_order_acquire)&&!atomic_load_explicit(&fault,memory_order_acquire);}
/* Privileged host owner is the sole producer; EL0 recognition task consumes. */
int mimus_keyboard_offer(const struct mimus_keyboard_report *p)
{
    if(atomic_load_explicit(&fault,memory_order_acquire))return -1;
    if(!mimus_keyboard_empty())return 0;
    if(!p||!p->generation||!p->sequence||!p->address||p->address>127||p->speed<1||p->speed>3||
       p->interface||p->protocol!=1||p->subclass!=1||p->mode||p->bytes!=8||!(p->endpoint&0x80)||
       !(p->endpoint&15)||(p->endpoint&0x70))return fail();
    mailbox=*p;atomic_store_explicit(&full,1,memory_order_release);return 1;
}
/* No providers, MMIO or privileged operations are reachable from this gate. */
int mimus_keyboard_step(void)
{
    if(atomic_load_explicit(&fault,memory_order_acquire))return -1;
    if(atomic_load_explicit(&accepted,memory_order_acquire))return 1;
    if(!atomic_load_explicit(&full,memory_order_acquire))return 0;
    struct mimus_keyboard_report p=mailbox;
    if(previous.sequence&&(p.generation!=previous.generation||p.address!=previous.address||p.speed!=previous.speed||
       p.interface!=previous.interface||p.protocol!=previous.protocol||p.endpoint!=previous.endpoint||
       p.subclass!=previous.subclass||p.mode!=previous.mode||previous.sequence==UINT64_MAX||p.sequence!=previous.sequence+1))return fail();
    if(p.data[1])return fail();
    for(unsigned i=2;i<8;++i){
        if(p.data[i]&&p.data[i]<4)return fail();
        for(unsigned j=2;j<i;++j)if(p.data[i]&&p.data[i]==p.data[j])return fail();
    }
    previous=p;
    unsigned zero=1;for(unsigned i=0;i<8;++i)if(p.data[i])zero=0;
    if(zero){baseline=p;neutral=1;return consumed(0);}
    unsigned enter=!p.data[0]&&p.data[2]==0x28;
    for(unsigned i=3;i<8;++i)if(p.data[i])enter=0;
    if(!neutral||!enter){neutral=0;return consumed(0);}
    put32(evidence,UINT32_C(0x4752544b));put32(evidence+4,1);put32(evidence+8,128);put32(evidence+12,2);
    put64(evidence+16,p.generation);evidence[24]=p.address;evidence[25]=p.speed;evidence[26]=p.interface;
    evidence[27]=p.protocol;evidence[28]=p.endpoint;evidence[29]=p.subclass;evidence[30]=p.mode;
    put64(evidence+32,baseline.sequence);memcpy(evidence+40,baseline.data,8);
    put64(evidence+80,p.sequence);memcpy(evidence+88,p.data,8);
    atomic_store_explicit(&accepted,1,memory_order_release);return consumed(1);
}
int mimus_keyboard_copy(uint8_t out[128])
{
    if(!out||atomic_load_explicit(&fault,memory_order_acquire))return -1;
    if(!atomic_load_explicit(&accepted,memory_order_acquire))return 0;
    memcpy(out,evidence,128);return 1;
}

int mimus_keyboard_rearm(void)
{
    if(atomic_load_explicit(&fault,memory_order_acquire)||
       !atomic_load_explicit(&accepted,memory_order_acquire)||
       atomic_load_explicit(&full,memory_order_acquire))return 0;
    /* Preserve previous identity/sequence, but require a new neutral report.
     * accepted keeps the producer out until this release publication. */
    neutral=0;
    atomic_store_explicit(&accepted,0,memory_order_release);return 1;
}
