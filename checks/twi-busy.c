/* SPDX-License-Identifier: GPL-2.0-only */
/* Registry BUSY injection for the native TWI11 adapter and the shared TWI
 * engine. The actual adapter, engine and pin tables run against modeled MMIO,
 * an SCCB target, registry/ledger status and platform mutexes. Each scenario
 * runs in its own process because the adapter admits one instance.
 * Build with -DSUNSTONE_CAMERA_MODEL=1 and the FreeRTOS include paths. */
#define _GNU_SOURCE
#include "a733_twi.h"
#include "a733_platform_locks.h"
#include "a733_counter_clock.h"
#include "task.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CCU UINT32_C(0x02002000)
#define TASK ((void *)0x1234)
static struct a733_counter_clock counter;
static struct a733_platform_locks locks;
static struct a733_clock_leases clocks;
static struct a733_rail_leases rails;
static struct a733_rail_borrow borrow={.id=7};
static struct a733_pins pins;
static struct a733_twi_native native;
static struct a733_twi engine;
static uint64_t now_us=100;
static unsigned held[A733_PLATFORM_LOCK_COUNT];
/* Status calls numbered from 1; calls busy_from..busy_from+busy_count-1 see
 * BUSY. refuse_at makes one call report an invalid token. */
static unsigned status_calls,busy_from,busy_count,refuse_at,busy_seen;
static struct { uintptr_t address; uint32_t value; } words[64],trace[256];
static unsigned word_count,trace_count,wire,stored_before_busy;
static uint8_t target[65536];
static uint16_t selected;

_Noreturn void platform_panic(const char *file,unsigned line){fprintf(stderr,"assert %s:%u\n",file,line);abort();}
int a733_counter_clock_now(void *context,uint64_t *out)
{assert(context==&counter);*out=++now_us;return 1;}
int a733_camera_model_current(void *task){return task==TASK;}
enum a733_clock_result a733_clock_token_status(struct a733_clock_leases *m,const struct a733_clock_token *t)
{
    assert(m==&clocks && t);++status_calls;
    if(refuse_at && status_calls==refuse_at)return A733_CLOCK_INVALID;
    if(busy_from && status_calls>=busy_from && status_calls<busy_from+busy_count){++busy_seen;return A733_CLOCK_BUSY;}
    return A733_CLOCK_OK;
}
enum a733_rail_result a733_rail_borrow_status(struct a733_rail_leases *m,const struct a733_rail_borrow *b)
{assert(m==&rails && b==&borrow);return A733_RAIL_READY;}
enum a733_pins_result a733_pins_held_banked(struct a733_pins *p,const struct a733_pin_lease_banked *l,int *owned,uint64_t d)
{(void)l;(void)d;assert(p==&pins);*owned=1;return A733_PINS_OK;}
int a733_platform_lock_enter(struct a733_platform_locks *p,enum a733_platform_lock which,uint64_t d)
{
    assert(p==&locks && d>now_us);
    for(unsigned i=0;i<A733_PLATFORM_LOCK_COUNT;i++)if(held[i])return 0;
    if(atomic_load(&locks.retained)&(1u<<which))return 0;
    held[which]=1;return 1;
}
void a733_platform_lock_leave(struct a733_platform_locks *p,enum a733_platform_lock which)
{assert(p==&locks && held[which] && !(atomic_load(&locks.retained)&(1u<<which)));held[which]=0;}
int a733_platform_lock_retain(struct a733_platform_locks *p,enum a733_platform_lock which)
{assert(p==&locks && held[which]);atomic_fetch_or(&locks.retained,1u<<which);return 1;}
TaskHandle_t xQueueGetMutexHolder(QueueHandle_t q)
{
    for(unsigned i=0;i<A733_PLATFORM_LOCK_COUNT;i++)if(q==locks.mutex[i])return held[i]?(TaskHandle_t)TASK:NULL;
    assert(0);return NULL;
}
static uint32_t *word(uintptr_t a)
{
    for(unsigned i=0;i<word_count;i++)if(words[i].address==a)return &words[i].value;
    assert(word_count<64);words[word_count].address=a;words[word_count].value=0;
    return &words[word_count++].value;
}
uint32_t a733_camera_model_load(uintptr_t a){assert(!(a&3));return *word(a);}
/* SCCB target at seven-bit address 0x3c with a two-byte register address. */
void a733_camera_model_store(uintptr_t a,uint32_t v)
{
    assert(!(a&3) && trace_count<256);
    trace[trace_count].address=a;trace[trace_count++].value=v;
    if(a!=A733_TWI11_BASE+0xc || v==0x40){*word(a)=v;return;}
    assert(held[A733_PLATFORM_I2C_LOCK] && !held[A733_PLATFORM_REGISTER_LOCK]);
    uint32_t data=*word(A733_TWI11_BASE+8),status=0;
    if(v==0x58){wire=0;*word(a)=0x40;*word(A733_TWI11_BASE+0x10)=0xf8;return;}
    if(v==0x68)status=wire?0x10:8;
    else switch(wire){
    case 1:assert(data==0x78);status=0x18;break;
    case 2:selected=(uint16_t)(data<<8);status=0x28;break;
    case 3:selected|=(uint16_t)data;status=0x28;break;
    case 4:target[selected]=(uint8_t)data;status=0x28;break;
    case 5:assert(data==0x79);status=0x40;break;
    case 6:*word(A733_TWI11_BASE+8)=target[selected];status=v==0x4c?0x50:0x58;break;
    default:assert(0);
    }
    ++wire;*word(a)=(v&~UINT32_C(0x30))|8;*word(A733_TWI11_BASE+0x10)=status;
}
static void setup(void)
{
    atomic_init(&counter.failed,0);counter.initialized=1;counter.frequency=24000000;
    for(unsigned i=0;i<A733_PLATFORM_LOCK_COUNT;i++)locks.mutex[i]=(SemaphoreHandle_t)(uintptr_t)(0x100+i);
    atomic_init(&locks.retained,0);locks.now=a733_counter_clock_now;locks.context=&counter;locks.initialized=1;
    clocks.now=a733_counter_clock_now;clocks.context=&counter;clocks.initialized=1;
    *word(0x07090160)=0x10000002;*word(CCU)=0xf8000000u|(99u<<8)|(99u<<16);
    for(unsigned p=7;p<=8;p++){
        struct a733_pin_fields f;assert(a733_pin_fields_banked(A733_PIN_E,p,&f));
        *word(f.mux_address)|=8u<<f.mux_shift;
    }
    *word(A733_TWI11_BASE+0x10)=0xf8;*word(A733_TWI11_BASE+0x20)=0x30;
    struct a733_twi_native_config c={.clocks=&clocks,.pins=&pins,.rails=&rails,.rail=&borrow,
        .locks=&locks,.task=TASK,.apb1=0,.apb1_hz=24000000};
    c.clock=(struct a733_clock_token){.id=1,.owner=50,.exclusive=A733_CLOCK_TWI11,
        .shared=A733_CLOCK_FABRIC|A733_CLOCK_REF|A733_CLOCK_OSC};
    c.pin.id=2;c.pin.owner=50;c.pin.masks[A733_PIN_E]=0x180;
    assert(a733_twi_native_init(&native,&c));
}
static int prepare(void)
{
    int r=0;
    for(unsigned i=0;i<64 && !r;i++){
        unsigned before=trace_count;
        r=a733_twi_native_prepare(&native,1000000000);
        /* A pending prepare leaves the register mutex without any store. */
        if(!r){assert(!held[A733_PLATFORM_REGISTER_LOCK] && trace_count==before);++stored_before_busy;}
    }
    return r;
}
static int transfer(unsigned reading,uint8_t *value)
{
    if(a733_twi_request(&engine,0x3c,0x3820,reading,*value,now_us+20000)!=0)return -1;
    /* Polls continue until completion or a fault; the deadline bounds them. */
    int r=0;for(unsigned i=0;i<1000000 && !r;i++)r=a733_twi_poll(&engine);
    assert(r);
    return r==1?a733_twi_take(&engine,value):r;
}
/* One write and one read of the same register; output is the store trace. */
static int scenario(void)
{
    setup();
    int r=prepare();if(r!=1)return r;
    if(!a733_twi_init(&engine,&native.operations,&native,native.divider))return -1;
    uint8_t value=0x5a;if(transfer(0,&value)!=1)return -1;
    value=0;if(transfer(1,&value)!=1 || value!=0x5a || target[0x3820]!=0x5a)return -1;
    return 1;
}
struct outcome { int result; unsigned retained,calls,busy_seen,trace_count,pending,fault; };
static struct outcome child_run(unsigned from,unsigned count,unsigned refuse,uintptr_t *out_trace,uint32_t *out_values)
{
    int pipefd[2];assert(!pipe(pipefd));
    pid_t pid=fork();assert(pid>=0);
    if(!pid){
        busy_from=from;busy_count=count;refuse_at=refuse;
        struct outcome o={0};o.result=scenario();
        o.retained=atomic_load(&locks.retained);o.calls=status_calls;o.busy_seen=busy_seen;
        o.trace_count=trace_count;o.pending=stored_before_busy;o.fault=native.fault;
        assert(write(pipefd[1],&o,sizeof(o))==sizeof(o));
        for(unsigned i=0;i<trace_count;i++){
            assert(write(pipefd[1],&trace[i].address,sizeof(uintptr_t))==sizeof(uintptr_t));
            assert(write(pipefd[1],&trace[i].value,sizeof(uint32_t))==sizeof(uint32_t));
        }
        _exit(0);
    }
    close(pipefd[1]);struct outcome o;
    assert(read(pipefd[0],&o,sizeof(o))==sizeof(o));
    for(unsigned i=0;i<o.trace_count;i++){
        assert(read(pipefd[0],&out_trace[i],sizeof(uintptr_t))==sizeof(uintptr_t));
        assert(read(pipefd[0],&out_values[i],sizeof(uint32_t))==sizeof(uint32_t));
    }
    close(pipefd[0]);int status;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    return o;
}
int main(void)
{
    static uintptr_t base_a[256],a[256];static uint32_t base_v[256],v[256];
    struct outcome base=child_run(0,0,0,base_a,base_v);
    assert(base.result==1 && !base.retained && !base.fault && base.calls>20);
    unsigned cases=0,pending=0;
    /* BUSY at every status call, for one and for eight consecutive calls,
     * across prepare and both transactions: the same stores occur once each,
     * and no mutex is retained. */
    for(unsigned run=1;run<=8;run+=7)for(unsigned n=1;n<=base.calls;n++){
        struct outcome o=child_run(n,run,0,a,v);
        if(o.result!=1 || o.retained || o.fault || !o.busy_seen || o.trace_count!=base.trace_count){
            fprintf(stderr,"BUSY at call %u x%u: result %d retained %u fault %u stores %u/%u\n",
                n,run,o.result,o.retained,o.fault,o.trace_count,base.trace_count);
            return 1;
        }
        for(unsigned i=0;i<o.trace_count;i++)assert(a[i]==base_a[i] && v[i]==base_v[i]);
        pending+=o.pending;++cases;
    }
    assert(pending>0);
    /* A real refusal still faults: after the first store it retains the held
     * mutex, and inside a transaction it retains I2C. */
    unsigned refused_retained=0;
    for(unsigned n=1;n<=base.calls;n++){
        struct outcome o=child_run(0,0,n,a,v);
        assert(o.result!=1);
        if(o.retained)++refused_retained;
        assert(!(o.retained&~((1u<<A733_PLATFORM_REGISTER_LOCK)|(1u<<A733_PLATFORM_I2C_LOCK))));
    }
    assert(refused_retained>0);
    /* BUSY that outlasts the deadline is bounded and ends in a fault. */
    struct outcome o=child_run(base.calls-6,1000000,0,a,v);
    assert(o.result!=1 && o.busy_seen>1000);
    printf("PASS TWI BUSY retry: %u injected cases, %u pending prepares without stores, %u stores unchanged; "
        "%u refusals retained, bounded BUSY faulted after %u observations\n",
        cases,pending,base.trace_count,refused_retained,o.busy_seen);
    return 0;
}
