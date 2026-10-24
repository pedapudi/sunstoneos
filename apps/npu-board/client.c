#include "client.h"
void npu_board_client_thermal(struct npu_board_client *c,int allow,int stop,uint64_t until,uint64_t now)
{
    uint64_t old=atomic_load_explicit(&c->admission,memory_order_acquire),next;
    do {
        if(old&NPU_BOARD_STOP)return;
        if(stop) {
            until=now<=UINT64_MAX-5000000 ? now+5000000 : c->session_end;
            if(until>c->session_end)until=c->session_end;
        }
        if(until>UINT64_MAX>>3)return;
        next=(until<<3)|(old&NPU_BOARD_ACCEPTED)|(stop?NPU_BOARD_STOP:allow?NPU_BOARD_ALLOW:0);
    } while(!atomic_compare_exchange_weak_explicit(&c->admission,&old,next,memory_order_acq_rel,memory_order_acquire));
    if(stop&&c->task)xTaskNotifyGive(c->task);
    if(c->bootstrap)xTaskNotifyGive(c->bootstrap);
}
uint64_t npu_board_client_watch(struct npu_board_client *c,uint64_t now)
{
    uint64_t word=atomic_load_explicit(&c->admission,memory_order_acquire),until=word>>3;
    for(unsigned i=0;i<8&&!(word&NPU_BOARD_STOP)&&now>=until&&now-until>=4000000;++i) {
        uint64_t end=now<=UINT64_MAX-5000000?now+5000000:c->session_end;
        if(end>c->session_end)end=c->session_end;
        const uint64_t next=(end<<3)|(word&NPU_BOARD_ACCEPTED)|NPU_BOARD_STOP;
        if(atomic_compare_exchange_weak_explicit(&c->admission,&word,next,memory_order_acq_rel,memory_order_acquire)) {
            if(c->task)xTaskNotifyGive(c->task);
            return next;
        }
        until=word>>3;
    }
    return word;
}
int npu_board_client_stopping(const struct npu_board_client *c)
{return (atomic_load_explicit(&c->admission,memory_order_acquire)&NPU_BOARD_STOP)!=0;}
void npu_board_client_complete(struct npu_board_client *c)
{atomic_fetch_and_explicit(&c->admission,~NPU_BOARD_ACCEPTED,memory_order_release);}
int npu_board_client_claim(struct npu_board_client *c)
{
    uint64_t word=atomic_load_explicit(&c->admission,memory_order_acquire),now;
    for(unsigned i=0;i<8;++i) {
        if((word&7)!=NPU_BOARD_ALLOW||!c->now||!c->now(c->clock,&now)||now>=word>>3)return 0;
        if(atomic_compare_exchange_weak_explicit(&c->admission,&word,word|NPU_BOARD_ACCEPTED,
            memory_order_acq_rel,memory_order_acquire)) {
            if(c->now(c->clock,&now)&&now<word>>3)return 1;
            npu_board_client_complete(c);return 0;
        }
    }
    return 0;
}
static void answer(struct npu_board_client *c,unsigned sequence,unsigned result)
{atomic_store_explicit(&c->answer,(sequence<<8)|result,memory_order_release);}
/* One runtime request and its reply; the reply queue wait is finite. */
static int call(struct npu_board_client *c,enum npu_service_operation op,unsigned model,uint64_t timeout,
    struct npu_service_reply *reply)
{
    uint64_t ticket;
    if(!npu_runtime_send(c->rt,c->slot,op,model,timeout,c->wait,&ticket))return 0;
    ++c->requests;
    if(!npu_runtime_collect(c->rt,c->slot,ticket,reply,c->wait))return 0;
    ++c->replies;
    return reply->ticket==ticket;
}
/* After a LOAD the tensor windows of the loaded model; USB0 reads these only
 * while a window is lent. */
static int tensors(struct npu_board_client *c)
{
    c->inputs=c->outputs=0;
    for(unsigned k=0;k<2;++k) {
        for(unsigned i=0;i<NBG_ASSEMBLY_MAX_TENSORS;++i) {
            struct npu_runtime_tensor t;
            if(npu_runtime_tensor(c->rt,c->slot,k ? NBG_OUTPUT : NBG_INPUT,i,&t)!=1)break;
            struct npu_board_tensor *out=k ? &c->output[c->outputs++] : &c->input[c->inputs++];
            *out=(struct npu_board_tensor){(uintptr_t)t.data,(uint32_t)t.bytes};
        }
    }
    return c->inputs && c->outputs;
}
void npu_board_client_run(struct npu_board_client *c)
{
    unsigned handled=0;
    struct npu_service_reply r;
    /* OPEN once the bootstrap has activated the service; USB0 answers every
     * request not-ready until ready is published. */
    while(!atomic_load_explicit(&c->start,memory_order_acquire))(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    if(!call(c,NPU_SERVICE_OPEN,0,c->job_timeout_us,&r) || r.result!=NPU_SERVICE_OK)return;
    c->state=r.state;
    atomic_store_explicit(&c->ready,1,memory_order_release);
    for(;;) {
        /* Automatic CLOSE changes only the atomic closed publication. USB0
         * may read its last plain snapshot without a posted request. */
        if(npu_board_client_stopping(c)&&!atomic_load_explicit(&c->closed,memory_order_acquire)&&
           atomic_load_explicit(&c->window,memory_order_acquire)==NPU_BOARD_HELD) {
            if(!call(c,NPU_SERVICE_CLOSE,0,0,&r)||r.result!=NPU_SERVICE_OK)break;
            atomic_store_explicit(&c->closed,1,memory_order_release);
            if(c->bootstrap)xTaskNotifyGive(c->bootstrap);
        }
        const unsigned q=atomic_load_explicit(&c->request,memory_order_acquire),sequence=q>>8;
        if(sequence==handled){(void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);continue;}
        handled=sequence;
        const unsigned op=q&255;
        if(npu_board_client_stopping(c)||atomic_load_explicit(&c->closed,memory_order_acquire)) {
            answer(c,sequence,NPU_BOARD_RESULT_REFUSED);continue;
        }
        const unsigned window=atomic_load_explicit(&c->window,memory_order_acquire);
        if(op==NPU_BOARD_RETURN) {
            /* The write window came back; lend the read window for the next
             * input. */
            if(window!=NPU_BOARD_HELD || (c->state!=A733_NPU_READY && c->state!=A733_NPU_DONE)){++c->refused;answer(c,sequence,NPU_BOARD_RESULT_REFUSED);continue;}
            atomic_store_explicit(&c->window,NPU_BOARD_READ_LENT,memory_order_release);
            answer(c,sequence,NPU_BOARD_RESULT_OK);continue;
        }
        /* SUBMIT, LOAD and CLOSE need both windows back with the client. */
        if(window!=NPU_BOARD_HELD){++c->refused;answer(c,sequence,NPU_BOARD_RESULT_REFUSED);continue;}
        if(op==NPU_BOARD_LOAD) {
            if(!call(c,NPU_SERVICE_LOAD,c->argument,0,&r))break;
            if(r.result!=NPU_SERVICE_OK){answer(c,sequence,NPU_BOARD_RESULT_REFUSED);continue;}
            c->state=r.state;c->model=c->argument;
            if(!tensors(c))break;
            atomic_store_explicit(&c->window,NPU_BOARD_READ_LENT,memory_order_release);
            answer(c,sequence,NPU_BOARD_RESULT_OK);
        } else if(op==NPU_BOARD_SUBMIT) {
            if(!npu_board_client_claim(c)){++c->refused;answer(c,sequence,NPU_BOARD_RESULT_REFUSED);continue;}
            if(!call(c,NPU_SERVICE_SUBMIT,0,c->job_timeout_us,&r) || r.result!=NPU_SERVICE_OK)break;
            npu_board_client_complete(c);
            c->state=r.state;c->job=r.ticket;
            atomic_store_explicit(&c->window,NPU_BOARD_WRITE_LENT,memory_order_release);
            answer(c,sequence,NPU_BOARD_RESULT_OK);
        } else if(op==NPU_BOARD_CLOSE) {
            if(!call(c,NPU_SERVICE_CLOSE,0,0,&r) || r.result!=NPU_SERVICE_OK)break;
            c->state=r.state;answer(c,sequence,NPU_BOARD_RESULT_OK);
            /* The bootstrap runs the stop table once notified. */
            atomic_store_explicit(&c->closed,1,memory_order_release);
            if(c->bootstrap)xTaskNotifyGive(c->bootstrap);
        } else break;
    }
    answer(c,handled,NPU_BOARD_RESULT_FAULT);
}
void npu_board_client_start(struct npu_board_client *c)
{atomic_store_explicit(&c->start,1,memory_order_release);xTaskNotifyGive(c->task);}
int npu_board_client_ready(const struct npu_board_client *c)
{return atomic_load_explicit(&c->ready,memory_order_acquire)!=0;}
int npu_board_client_post(struct npu_board_client *c,unsigned sequence,enum npu_board_operation op,unsigned argument)
{
    /* USB0 is the only writer of request, so this check cannot race. */
    const unsigned q=atomic_load_explicit(&c->request,memory_order_relaxed);
    if(!npu_board_client_ready(c) || (npu_board_client_answer(c,q>>8)==0 && (q>>8)))return 0;
    c->argument=argument;
    atomic_store_explicit(&c->request,(sequence<<8)|op,memory_order_release);
    xTaskNotifyGive(c->task);
    return 1;
}
unsigned npu_board_client_answer(const struct npu_board_client *c,unsigned sequence)
{
    const unsigned a=atomic_load_explicit(&c->answer,memory_order_acquire);
    return (a>>8)==sequence ? (a&255) : 0;
}
void npu_board_client_return(struct npu_board_client *c)
{atomic_store_explicit(&c->window,NPU_BOARD_HELD,memory_order_release);xTaskNotifyGive(c->task);}
