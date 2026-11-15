/* Host model of the shared device-service fault report (apps/os/device-fault.h).
 *
 * The owner task's sequence and one quiesce step run on two threads that hold a baton: exactly one runs at a time,
 * and the baton passes only at the helper's atomic operations, which this file hooks. A placement names, for each of
 * the step's first four atomic operations, the number of owner operations that come before it. Every placement is
 * run, so the step's loads fall at every point among the owner's stores, and between two of its own loads any number
 * of owner stores can run. Plain fields are read and written where the thread holding the baton reaches them.
 *
 * Scenarios, each from a service whose first fault is step 4, value 0x123456 (an open service for open-closes):
 * - settles: the fault is one a close can settle; the owner task's close succeeds and the service reports quiesced.
 * - close-fails: the close fails with step 9; the service stays FAULT and publishes its first fault.
 * - unsettled: the fault is one no close can settle; the service publishes it at once and is not closed.
 * - open-closes: an open service, with no fault, closes on the quiesce.
 * The concurrent step may report FAULT only in close-fails and unsettled, and then only with the first fault's value
 * and the service's site. A step after the owner finishes returns DONE in settles and open-closes, FAULT otherwise.
 * Every store of reported uses release order and every load of it acquire order: the host runs without reordering,
 * so the model asserts the orders written in the source. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

enum who { OWNER, STEP };
#define PLACED 4u
#define AFTER_OWNER 0xffffffffu

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t turn = PTHREAD_COND_INITIALIZER;
static _Thread_local enum who me = OWNER;
static enum who baton;
static unsigned done[2], finished[2], place[PLACED];
static atomic_uint *reported_word;
static unsigned order_faults, notified;

static unsigned step_position(void){return done[STEP]<PLACED?place[done[STEP]]:AFTER_OWNER;}
/* The thread that runs after an operation (or at a thread's end). */
static enum who next(void)
{
    if(finished[OWNER])return STEP;
    if(finished[STEP])return OWNER;
    return step_position()<=done[OWNER]?STEP:OWNER;
}
static void wait_turn(void){while(baton!=me)pthread_cond_wait(&turn,&lock);}
static void hand_on(void){baton=next();pthread_cond_broadcast(&turn);wait_turn();}
static void order_check(atomic_uint *p, memory_order o, int store)
{
    if(p!=reported_word)return;
    if(store&&o!=memory_order_release){fprintf(stderr,"reported stored with order %d\n",(int)o);order_faults++;}
    if(!store&&o!=memory_order_acquire){fprintf(stderr,"reported loaded with order %d\n",(int)o);order_faults++;}
}
static unsigned model_load(atomic_uint *p, memory_order o)
{
    pthread_mutex_lock(&lock);wait_turn();
    order_check(p,o,0);
    const unsigned v=__atomic_load_n((unsigned *)p,__ATOMIC_SEQ_CST);
    done[me]++;hand_on();
    pthread_mutex_unlock(&lock);
    return v;
}
static void model_store(atomic_uint *p, unsigned v, memory_order o)
{
    pthread_mutex_lock(&lock);wait_turn();
    order_check(p,o,1);
    __atomic_store_n((unsigned *)p,v,__ATOMIC_SEQ_CST);
    done[me]++;hand_on();
    pthread_mutex_unlock(&lock);
}
#undef atomic_load_explicit
#undef atomic_store_explicit
#define atomic_load_explicit(p,o) model_load((p),(o))
#define atomic_store_explicit(p,v,o) model_store((p),(v),(o))
#include "device-fault.h"

/* The quiesce registry's fault capture and the owner's notification. */
static uint32_t noted_site, noted_value;
int os_quiesce_fault(struct os_quiesce *q, uint32_t site, uint32_t value, const void *caller)
{(void)q;(void)caller;noted_site=site;noted_value=value;return 1;}
BaseType_t xTaskGenericNotify(TaskHandle_t t, UBaseType_t index, uint32_t value, eNotifyAction action, uint32_t *previous)
{(void)t;(void)index;(void)value;(void)action;(void)previous;notified++;return pdPASS;}

enum scenario { SETTLES, CLOSE_FAILS, UNSETTLED, OPEN_CLOSES, SCENARIOS };
static const char *const names[SCENARIOS] = {"settles", "close-fails", "unsettled", "open-closes"};
#define SITE UINT32_C(0x941)
#define FIRST_STEP 4u
#define FIRST_VALUE UINT32_C(0x123456)
#define EXPECTED (FIRST_STEP << 24 | FIRST_VALUE)

static int fake_task;
static TaskHandle_t owner_handle = (TaskHandle_t)&fake_task;
static struct os_device_fault dev;
static struct os_quiesce quiesce;

/* The owner task: the failure, then the service's quiesce lines (apps/os/video_decode.c, os_video_decode_task). */
static void owner(enum scenario s)
{
    if(s==OPEN_CLOSES)atomic_store_explicit(&dev.phase,OS_DEVICE_OPEN,memory_order_seq_cst);
    else (void)os_device_fail(&dev,FIRST_STEP,FIRST_VALUE,s==UNSETTLED);
    if(!os_device_closable(&dev,s!=UNSETTLED))return;
    (void)os_device_close_begin(&dev);
    if(s==CLOSE_FAILS){(void)os_device_fail(&dev,9,0xabc,0);return;}
    atomic_store_explicit(&dev.phase,OS_DEVICE_CLOSED,memory_order_seq_cst);
    if(atomic_load_explicit(&dev.phase,memory_order_seq_cst)==OS_DEVICE_CLOSED)
        atomic_store_explicit(&dev.quiesced,1,memory_order_seq_cst);
}

static enum os_quiesce_result concurrent;
static void *step_thread(void *unused)
{
    (void)unused;
    me=STEP;
    concurrent=os_device_quiesce_step(&dev,&quiesce);
    pthread_mutex_lock(&lock);finished[STEP]=1;baton=next();pthread_cond_broadcast(&turn);pthread_mutex_unlock(&lock);
    return NULL;
}
static void reset(void)
{
    memset(&dev,0,sizeof(dev));dev.site=SITE;dev.owner=&owner_handle;
    memset(&quiesce,0,sizeof(quiesce));
    memset(done,0,sizeof(done));memset(finished,0,sizeof(finished));
    noted_site=noted_value=0;reported_word=&dev.reported;
}
/* One placement; with step 0 the owner runs alone. Returns the number of owner operations. */
static unsigned run(enum scenario s, int with_step, const unsigned *at)
{
    reset();
    pthread_t t;
    if(with_step){
        memcpy(place,at,sizeof(place));
        baton=place[0]==0?STEP:OWNER;
        if(pthread_create(&t,NULL,step_thread,NULL)){perror("pthread_create");exit(2);}
    }else{finished[STEP]=1;baton=OWNER;}
    me=OWNER;
    owner(s);
    pthread_mutex_lock(&lock);finished[OWNER]=1;baton=next();pthread_cond_broadcast(&turn);pthread_mutex_unlock(&lock);
    if(with_step)pthread_join(t,NULL);
    return done[OWNER];
}
static int faultable(enum scenario s){return s==CLOSE_FAILS||s==UNSETTLED;}
static int bad_report(void)
{
    return noted_site!=(SUNSTONE_CRUMB_FAIL_RETURN|SUNSTONE_CRUMB_FAIL_BOARD<<12|SITE)||noted_value!=EXPECTED;
}

int main(void)
{
    unsigned failures=0, placements=0;
    for(enum scenario s=0;s<SCENARIOS;s++){
        const unsigned n=run(s,0,NULL);
        unsigned seen[3]={0,0,0};
        unsigned at[PLACED];
        for(at[0]=0;at[0]<=n;at[0]++)for(at[1]=at[0];at[1]<=n;at[1]++)
        for(at[2]=at[1];at[2]<=n;at[2]++)for(at[3]=at[2];at[3]<=n;at[3]++){
            run(s,1,at);placements++;
            seen[concurrent]++;
            int bad=0;
            if(concurrent==OS_QUIESCE_FAULT&&(!faultable(s)||bad_report()))bad=1;
            if(concurrent==OS_QUIESCE_DONE&&faultable(s))bad=1;
            const uint32_t concurrent_value=noted_value;
            /* The step the supervisor makes on its next pass, after the owner task has finished. */
            noted_site=noted_value=0;
            me=STEP;
            const enum os_quiesce_result last=os_device_quiesce_step(&dev,&quiesce);
            me=OWNER;
            if(faultable(s)?last!=OS_QUIESCE_FAULT||bad_report():last!=OS_QUIESCE_DONE)bad=1;
            if(bad){
                failures++;
                if(failures<=8)fprintf(stderr,"%s: placement %u %u %u %u: concurrent %d (value 0x%x), then %d (value 0x%x)\n",
                    names[s],at[0],at[1],at[2],at[3],(int)concurrent,concurrent_value,(int)last,noted_value);
            }
        }
        printf("%s: %u owner operations; concurrent step PENDING %u, DONE %u, FAULT %u\n",
            names[s],n,seen[OS_QUIESCE_PENDING],seen[OS_QUIESCE_DONE],seen[OS_QUIESCE_FAULT]);
    }
    printf("%u placements, %u failures, %u memory-order faults, %u notifications\n",placements,failures,order_faults,notified);
    return failures||order_faults?1:0;
}
