/* Run the actual mailbox with a blocking owner and persistent notifications. */
#include <assert.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <string.h>
#include "../../runtime/npu_app.c"
static sem_t requests,completed,waiting;
static atomic_uint notifications;
static unsigned calls,closes;
static char input[4]="abc",output[4];
static int run(unsigned model,const void *in,size_t ni,void *out,size_t no,uint64_t timeout)
{
    assert(model==7&&in==input&&ni==4&&out==output&&no==4&&timeout==100);
    memcpy(out,in,ni);++calls;return 1;
}
static int close_backend(void){++closes;return 1;}
static void notify(void *context)
{
    assert(context==&requests);
    /* The callback may inspect only the released publication. */
    unsigned state=atomic_load_explicit(&box.state,memory_order_acquire);
    assert(state==RUN||state==CLOSE||state==FAILED);
    atomic_fetch_add(&notifications,1);assert(!sem_post(context));
}
static void *owner(void *unused)
{
    (void)unused;
    for(;;) {
        assert(!sem_post(&waiting));assert(!sem_wait(&requests));
        int result=ss_npu_service_step();assert(result==1||result==-1);
        assert(!sem_post(&completed));if(result<0)return NULL;
    }
}
int main(void)
{
    assert(!sem_init(&requests,0,0)&&!sem_init(&completed,0,0)&&!sem_init(&waiting,0,0));
    assert(!ss_npu_service_bind_event(NULL,&requests));
    assert(ss_npu_service_bind_event(notify,&requests));
    assert(!ss_npu_service_bind_event(notify,&requests));
    const struct ss_npu_model models[]={
        {"m0",1,1,4,4},{"m1",1,1,4,4},{"m2",1,1,4,4},{"m3",1,1,4,4},
        {"m4",1,1,4,4},{"m5",1,1,4,4},{"m6",1,1,4,4},{"m7",1,1,4,4}};
    const struct ss_npu_backend backend={run,close_backend,models,8,NULL};
    assert(ss_npu_service_bind(&backend));assert(!ss_npu_service_bind_event(notify,&requests));
    /* Publication precedes the owner's wait: its event must remain pending. */
    assert(ss_npu_run(7,input,4,output,4,100)==0);
    assert(ss_npu_run(7,input,4,output,4,100)==0&&atomic_load(&notifications)==1);
    pthread_t task;assert(!pthread_create(&task,NULL,owner,NULL));
    assert(!sem_wait(&completed));assert(calls==1&&!memcmp(input,output,4));
    assert(ss_npu_run(7,input,4,output,4,100)==1);
    /* Drain both wait-entry markers; the next request arrives after waiting. */
    assert(!sem_wait(&waiting)&&!sem_wait(&waiting));
    assert(ss_npu_run(7,input,4,output,4,100)==0);
    assert(!sem_wait(&completed)&&calls==2&&atomic_load(&notifications)==2);
    assert(ss_npu_run(7,input,4,output,4,100)==1);
    assert(ss_npu_close()==0);assert(!sem_wait(&completed));
    assert(ss_npu_close()==1&&closes==1&&atomic_load(&notifications)==3);
    /* A lifecycle failure wakes an idle owner, once even when repeated. */
    ss_npu_service_fail();ss_npu_service_fail();
    assert(!sem_wait(&completed));assert(!pthread_join(task,NULL));
    assert(atomic_load(&notifications)==4&&ss_npu_service_step()==-1);
    assert(ss_npu_run(7,input,4,output,4,100)==-1);
    assert(!sem_destroy(&requests)&&!sem_destroy(&completed)&&!sem_destroy(&waiting));
}
