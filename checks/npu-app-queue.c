/* Execute the real producer/consumer with modeled hardware effects. */
#include "sunstone_npu_queue_service.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void idle_race(struct ss_npu_queue *);
#define SS_NPU_QUEUE_AFTER_IDLE_CLEAR(q) idle_race(q)
#include "../runtime/npu_app_queue.c"
#include "../drivers/npu/npu_queue.c"
static struct ss_npu_queue q;
static _Alignas(64) unsigned char input[2][64],output[2][64];
static unsigned starts,finishes,bells,cache_calls,requests,notifications,waits;
static unsigned running,race,fault,inside,closed,linked,admission_failure;
static struct npu_queue native_queue;
static struct npu_queue_command native_command[2];
static struct npu_queue_job native_job;
static struct npu_queue_fence native_fence;
static uintptr_t task=1;
static struct ss_npu_fence injected;
static uintptr_t identity(void *c){(void)c;return task;}
static int on_core(void *c){(void)c;return 1;}
static void request(void *c){(void)c;++requests;}
static int open_backend(void *c,unsigned depth)
{
    (void)c;assert(depth==3);
    for(unsigned i=0;i<2;++i){
        struct ss_npu_tensor a={linked&&i==1?output[0]:input[i],admission_failure==7&&i==1?32:64},b={output[i],64};
        assert(ss_npu_queue_service_handle(&q,&a,1,&b,1,
            (struct ss_npu_cache_span){(uintptr_t)input[i],64},
            (struct ss_npu_cache_span){(uintptr_t)output[i],64}));
    }
    if(linked){
        const struct ss_npu_queue_edge edges[2]={{0,0,1,0},{0,0,1,0}};
        if(admission_failure){
            if(admission_failure==4)assert(!ss_npu_queue_service_edges(&q,NULL,0));
            else if(admission_failure==5){
                const struct ss_npu_queue_edge bad={0,0,1,1};
                assert(!ss_npu_queue_service_edges(&q,&bad,1));
            } else assert(!ss_npu_queue_service_edges(&q,edges,admission_failure==6?2:1));
            return -1;
        }
        assert(ss_npu_queue_service_edges(&q,edges,1));
    }
    return 1;
}
static int close_backend(void *c){(void)c;++closed;return 1;}
static int validate_native(void *c,const struct a733_npu_command *cmd)
{(void)c;return cmd->device==1||cmd->device==2;}
static int start_native(void *c,const struct a733_npu_command *cmd,uint64_t timeout)
{
    (void)c;assert(!running&&timeout==100);++starts;running=cmd->device;
    return fault==1?-1:1;
}
static int complete_native(void *c){(void)c;return fault==2?-1:1;}
static void notify_native(void *c){(void)c;}
static int start(void *c,unsigned h,uint64_t seq,uint64_t after,uint64_t timeout)
{
    (void)c;assert(!running&&h&&h<=2&&seq==starts+1&&after<seq&&timeout==100);
    struct npu_queue_fence dep=after?(struct npu_queue_fence){&native_queue,after}:(struct npu_queue_fence){0};
    return npu_queue_submit(&native_queue,&native_job,&native_command[h-1],dep,timeout,&native_fence);
}
static int finish(void *c,uint64_t seq)
{
    (void)c;assert(!running&&seq==finishes+1);++finishes;
    return native_fence.value==seq&&npu_queue_fence_status(&native_queue,native_fence)==1&&
        npu_queue_release(&native_queue,&native_job)?1:-1;
}
static void cache(void *c,uintptr_t addr,size_t bytes)
{(void)c;assert(bytes==64&&!(addr&63));++cache_calls;}
static int doorbell(void *c)
{
    (void)c;++bells;
    if(inside)return 1;
    return ss_npu_queue_service_progress(&q,0,0);
}
static void notify(void *c,uintptr_t t,int isr)
{(void)c;(void)isr;assert(t==1);++notifications;}
static void complete(void)
{
    assert(running);unsigned h=running-1;
    memcpy(output[h],linked&&h==1?output[0]:input[h],64);running=0;inside=1;
    npu_queue_irq(&native_queue);
    (void)ss_npu_queue_service_progress(&q,1,1);inside=0;
}
static void wait(void *c){(void)c;++waits;complete();}
static void idle_race(struct ss_npu_queue *unused)
{
    (void)unused;
    if(race){race=0;assert(ss_npu_queue_submit(&q,1,(struct ss_npu_fence){0},100,&injected)==1);}
}
static void linked_case(void)
{
    ss_npu_handle a,b;struct ss_npu_tensor t;
    assert(ss_npu_queue_register(&q,0,&a)&&ss_npu_queue_register(&q,1,&b));
    assert(ss_npu_queue_tensor(&q,b,SS_NPU_INPUT,0,&t)&&t.data==output[0]);
    memset(input[0],71,64);
    struct ss_npu_fence f1,f2,f3,f4;
    assert(ss_npu_queue_submit(&q,a,(struct ss_npu_fence){0},100,&f1)==1);
    assert(cache_calls==3); /* Two outputs and the external input, each once. */
    assert(ss_npu_queue_submit(&q,b,f1,100,&f2)==1&&starts==1&&bells==1&&cache_calls==3);
    assert(!ss_npu_queue_tensor(&q,b,SS_NPU_OUTPUT,0,&t));
    assert(!ss_npu_queue_dirty(&q,a,SS_NPU_INPUT,0,0,1));
    assert(!ss_npu_queue_dirty(&q,b,SS_NPU_INPUT,0,0,1));
    complete();
    assert(starts==2&&cache_calls==3&&ss_npu_queue_status(&q,f1)==1);
    assert(ss_npu_queue_acquire(&q,f1)==0&&ss_npu_queue_acquire_handle(&q,a)==0);
    complete();
    assert(ss_npu_queue_acquire(&q,f1)==1&&cache_calls==5&&output[1][0]==71);
    assert(ss_npu_queue_acquire_handle(&q,b)==1&&cache_calls==5);
    assert(ss_npu_queue_release(&q,f1)&&ss_npu_queue_release(&q,f2));
    /* Writing the aliased input marks the owning producer output. */
    assert(ss_npu_queue_tensor(&q,b,SS_NPU_INPUT,0,&t));((unsigned char *)t.data)[0]=33;
    assert(ss_npu_queue_dirty(&q,b,SS_NPU_INPUT,0,0,1));
    assert(ss_npu_queue_submit(&q,b,f2,100,&f3)==1&&cache_calls==6);
    complete();assert(output[1][0]==33&&ss_npu_queue_release(&q,f3));
    /* Reuse a wrapped receipt for another handle without returning the lease. */
    assert(ss_npu_queue_submit(&q,a,f3,100,&f4)==1&&cache_calls==6);
    complete();assert(ss_npu_queue_release(&q,f4));
    assert(ss_npu_queue_acquire(&q,f1)==-1);
    assert(ss_npu_queue_acquire_handle(&q,b)==1&&cache_calls==8);
    assert(ss_npu_queue_tensor(&q,a,SS_NPU_OUTPUT,0,&t));
    assert(ss_npu_queue_close(&q)==0&&ss_npu_queue_service_step(&q)==1&&closed);
    puts("PASS declared alias, connected lease, dependency refill, dirty ownership and released receipt recovery");
}
int main(int argc,char **argv)
{
    if(argc>1&&atoi(argv[1])>=3)linked=1;
    if(argc>1&&atoi(argv[1])>=4)admission_failure=(unsigned)atoi(argv[1]);
    const struct ss_npu_queue_backend b={identity,on_core,request,open_backend,close_backend,
        start,finish,cache,doorbell,notify,wait};
    const struct npu_queue_ops ops={validate_native,start_native,complete_native,notify_native};
    assert(npu_queue_init(&native_queue,&ops,&q));
    for(unsigned i=0;i<2;++i){
        struct a733_npu_command cmd={.device=i+1};
        assert(npu_queue_register(&native_queue,&native_command[i],&cmd));
    }
    assert(ss_npu_queue_service_bind(&q,&b,NULL));
    /* The queue's tables come from memory the OS gives it, sized by the declaration: 4 handles, 16 tensors, 4 links. */
    static _Alignas(8) unsigned char tables[4096];
    assert(sizeof(tables)>=ss_npu_queue_tables_bytes(4,16,4)&&!ss_npu_queue_service_tables(&q,4,16,4,tables,64));
    assert(ss_npu_queue_service_tables(&q,4,16,4,tables,sizeof(tables)));
    struct ss_npu_queue *pub=NULL;
    if(argc>1&&atoi(argv[1])==8){
        /* The ring holds the depth the image was built for, which is what the application declared: one more is refused
         * and exactly that many is accepted, whatever that number is. */
        assert(ss_npu_queue_open(SS_NPU_QUEUE_MAX_DEPTH+1,&pub)==-1&&requests==0);
        assert(ss_npu_queue_open(SS_NPU_QUEUE_MAX_DEPTH,&pub)==0&&requests==1);
        printf("PASS a ring of %u entries is accepted and one more is refused\n",(unsigned)SS_NPU_QUEUE_MAX_DEPTH);return 0;
    }
    if(argc>1&&atoi(argv[1])==9){
        /* Links chosen at open: copied once into the queue for the backend, refused when malformed, ignored when polling. */
        const struct ss_npu_queue_edge want[2]={{0,0,1,0},{1,0,0,0}};
        assert(ss_npu_queue_open_linked(3,want,5,&pub)==-1&&requests==0);
        assert(ss_npu_queue_open_linked(3,NULL,1,&pub)==-1&&requests==0);
        assert(ss_npu_queue_open_linked(3,want,2,&pub)==0&&requests==1);
        assert(q.requested_links==2&&!memcmp(q.requested,want,sizeof(want)));
        struct ss_npu_queue_edge other={1,1,0,1};
        assert(ss_npu_queue_open_linked(3,&other,1,&pub)==0&&requests==1&&q.requested_links==2&&!memcmp(q.requested,want,sizeof(want)));
        puts("PASS links chosen at open reach the backend once and a malformed table is refused");return 0;
    }
    assert(ss_npu_queue_open(3,&pub)==0&&requests==1);
    if(admission_failure){
        assert(ss_npu_queue_service_step(&q)==-1&&!starts&&!cache_calls);
        assert(ss_npu_queue_open(3,&pub)==-1&&ss_npu_queue_close(&q)==-1);
        puts("PASS undeclared or malformed tensor alias refused before device/cache work");return 0;
    }
    assert(ss_npu_queue_service_step(&q)==1&&notifications==1);
    assert(ss_npu_queue_open(3,&pub)==1&&pub==&q);
    if(linked){linked_case();return 0;}
    ss_npu_handle a=0,bh=0;assert(ss_npu_queue_register(&q,0,&a)&&a==1);
    assert(ss_npu_queue_register(&q,1,&bh)&&bh==2);
    assert(!ss_npu_queue_register(&q,2,&bh));
    struct ss_npu_tensor tensor;
    assert(ss_npu_queue_tensor(&q,a,SS_NPU_INPUT,0,&tensor)&&tensor.data==input[0]);
    memset(input[0],17,64);memset(input[1],29,64);
    if(argc>1)fault=(unsigned)atoi(argv[1]);
    struct ss_npu_fence f1,f2,f3,unchanged={NULL,19};
    int first=ss_npu_queue_submit(&q,a,(struct ss_npu_fence){0},100,&f1);
    if(fault==1){assert(first==-1&&f1.value==1&&q.group[q.handle[0].group].device_owned);goto retained;}
    assert(first==1&&bells==1&&starts==1&&cache_calls==2);
    assert(!ss_npu_queue_submit(&q,a,(struct ss_npu_fence){&q,2},100,&unchanged)&&unchanged.value==19);
    assert(ss_npu_queue_submit(&q,bh,f1,100,&f2)==1);
    assert(ss_npu_queue_submit(&q,a,f2,100,&f3)==1&&bells==1&&cache_calls==4);
    assert(!ss_npu_queue_submit(&q,a,f3,100,&unchanged)&&unchanged.value==19);
    assert(!ss_npu_queue_tensor(&q,a,SS_NPU_OUTPUT,0,&tensor));
    assert(!ss_npu_queue_dirty(&q,a,SS_NPU_INPUT,0,0,1));
    assert(ss_npu_queue_close(&q)==0);
    assert(!ss_npu_queue_release(&q,f2));
    complete();
    if(fault==2){assert(q.group[q.handle[0].group].device_owned);goto retained;}
    assert(ss_npu_queue_status(&q,f1)==1&&ss_npu_queue_acquire(&q,f1)==0);
    assert(ss_npu_queue_release(&q,f1)==1);
    struct ss_npu_fence f4;
    assert(ss_npu_queue_submit(&q,a,f3,100,&f4)==1&&bells==1&&cache_calls==4);
    /* Complete only the requested final waiter; intermediate IRQs do not wake. */
    assert(ss_npu_queue_wait(&q,f4)==1&&waits==3&&notifications==2);
    assert(ss_npu_queue_acquire(&q,f2)==1&&output[1][0]==29);
    assert(ss_npu_queue_acquire(&q,f3)==1&&output[0][0]==17&&cache_calls==6);
    assert(ss_npu_queue_release(&q,f2)&&ss_npu_queue_release(&q,f3)&&ss_npu_queue_release(&q,f4));
    assert(ss_npu_queue_acquire(&q,f3)==-1);
    assert(ss_npu_queue_tensor(&q,a,SS_NPU_INPUT,0,&tensor));
    assert(!ss_npu_queue_dirty(&q,a,(enum ss_npu_tensor_kind)-1,0,0,1));
    memset(tensor.data,53,64);
    assert(!ss_npu_queue_dirty(&q,a,SS_NPU_INPUT,0,63,2));
    assert(ss_npu_queue_dirty(&q,a,SS_NPU_INPUT,0,0,64));
    struct ss_npu_fence f5;
    assert(ss_npu_queue_submit(&q,a,f4,100,&f5)==1&&bells==2&&cache_calls==7);
    race=1;complete(); /* Publication after idle clear is rechecked. */
    assert(injected.value==6&&running==1&&starts==6&&bells==3);
    complete();assert(ss_npu_queue_acquire(&q,injected)==1&&output[0][0]==53);
    assert(ss_npu_queue_release(&q,f5)&&ss_npu_queue_release(&q,injected));
    task=2;assert(ss_npu_queue_status(&q,f5)==-1);task=1;
    struct ss_npu_fence f7,f8;
    assert(ss_npu_queue_submit(&q,a,injected,100,&f7)==1);
    complete();assert(ss_npu_queue_release(&q,f7));
    assert(ss_npu_queue_submit(&q,bh,f7,100,&f8)==1);
    complete();assert(ss_npu_queue_release(&q,f8));
    /* Recycled receipts cannot strand independent handle leases either. */
    assert(ss_npu_queue_acquire(&q,f1)==-1);
    assert(ss_npu_queue_acquire_handle(&q,a)==1&&ss_npu_queue_acquire_handle(&q,bh)==1);
    q.job_limit=atomic_load(&q.published);
    assert(!ss_npu_queue_submit(&q,a,injected,100,&unchanged)&&unchanged.value==19);
    assert(ss_npu_queue_close(&q)==0&&requests==2);
    assert(ss_npu_queue_service_step(&q)==1&&closed==1&&ss_npu_queue_close(&q)==1);
    puts("PASS two resident handles, dependencies, backpressure, wrap, ownership, final wait and idle race");return 0;
retained:
    assert(ss_npu_queue_status(&q,f1)==-1&&ss_npu_queue_acquire(&q,f1)==-1);
    assert(!ss_npu_queue_release(&q,f1)&&ss_npu_queue_close(&q)==-1&&!closed);
    puts("PASS retained failure preserves receipts and device buffer leases");return 0;
}
