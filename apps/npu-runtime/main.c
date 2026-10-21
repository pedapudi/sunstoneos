#include "runtime.h"
#include "platform.h"
#include "boot.h"
#include "cached.h"
#include "result.h"
#include "session.h"
#include <string.h>
#include <limits.h>
#if !defined(SUNSTONE_NPU_SINGLE_CORE) || !defined(SUNSTONE_NPU_INTERRUPT) || defined(SUNSTONE_NPU_FINITE_RETURN)
#error "Runtime input requires retained single-core IRQ composition"
#endif
static StaticTask_t client_state;
static StackType_t client_stack[4096] __attribute__((aligned(16)));
struct a733_npu_application_result a733_npu_application_result;
static unsigned booting=1;
static int bounded(void)
{return npu_runtime_live()&&a733_smp_counter()<npu_runtime.deadline;}
static int request(struct npu_convolution *c,enum npu_service_operation op,const void *input,size_t bytes,
 struct npu_service_reply *reply)
{
 uint64_t ticket,now=a733_smp_counter();
 uint64_t until=booting?a733_observe_identity.deadline:npu_runtime.deadline;
 if(!npu_runtime_live()||now>=until)return 0;
 uint64_t timeout=(until-now)/24;if(!timeout)return 0;if(timeout>1000000)timeout=1000000;
 if(!npu_convolution_send(c,op,input,bytes,(op==NPU_SERVICE_OPEN||op==NPU_SERVICE_SUBMIT)?timeout:0,5,&ticket))return 0;
 for(;;){
  if(!npu_runtime_live()||a733_smp_counter()>=until)return 0;
  /* On timeout retain this exact ticket; no subsequent command is issued. */
  if(npu_convolution_collect(c,ticket,reply,5))break;
 }
 return !atomic_load_explicit(&c->failed,memory_order_acquire)&&reply->result==NPU_SERVICE_OK&&
  reply->fault==A733_NPU_NO_FAULT&&!reply->canceled&&
  (op==NPU_SERVICE_CLOSE?(!reply->owns_session&&!reply->job_ticket&&reply->state==A733_NPU_CLOSED):reply->owns_session);
}
static void client(void *unused)
{
 (void)unused;struct npu_service_reply reply;struct npu_convolution *c=NULL;
 while(!a733_npu_application_ready()){if(!npu_runtime_live())goto failed;vTaskDelay(1);}
 c=a733_npu_application_client();if(!c||!request(c,NPU_SERVICE_OPEN,NULL,0,&reply))goto failed;
 /* WAIT's held reply arrives when bootstrap or a job completes; no polling. */
 if(reply.state==A733_NPU_BOOTING&&!request(c,NPU_SERVICE_WAIT,NULL,0,&reply))goto failed;
 if(reply.state!=A733_NPU_READY)goto failed;
 if(npu_service_irq_completions(&c->service)!=1)goto failed;booting=0;
 for(;;){
  if(!npu_runtime_live())goto failed;
  unsigned expected=NR_QUEUED;
  if(!atomic_compare_exchange_strong_explicit(&npu_runtime.state,&expected,NR_RUNNING,memory_order_acq_rel,memory_order_acquire)){
   if(expected==NR_FAILED)goto failed;vTaskDelay(1);continue;
  }
  if(!bounded())goto failed;npu_runtime.start=a733_smp_counter();
  if(npu_runtime.operation==NPU_RUNTIME_CLOSE){
   if(!request(c,NPU_SERVICE_CLOSE,NULL,0,&reply))goto failed;
   while(!a733_npu_application_stopped()){if(!bounded())goto failed;vTaskDelay(1);}
   if(!npu_service_irq_stopped(&c->service)||npu_service_irq_completions(&c->service)!=npu_runtime.jobs+1)goto failed;
   npu_runtime.irq_count=npu_service_irq_completions(&c->service);npu_runtime.end=a733_smp_counter();
   expected=NR_RUNNING;
   if(!bounded()||!atomic_compare_exchange_strong_explicit(&npu_runtime.state,&expected,NR_CLOSED,memory_order_release,memory_order_relaxed))goto failed;
   vTaskSuspend(NULL);goto failed;
  }
  if(npu_runtime.jobs>=UINT_MAX-1||!request(c,NPU_SERVICE_SUBMIT,NPU_RUNTIME_INPUT,npu_runtime.bytes,&reply))goto failed;
  uint64_t job=reply.job_ticket;if(!job)goto failed;
  if(reply.state!=A733_NPU_DONE&&(reply.state!=A733_NPU_RUNNING||reply.job_ticket!=job||
     !request(c,NPU_SERVICE_WAIT,NULL,0,&reply)))goto failed;
  if(reply.state!=A733_NPU_DONE)goto failed;
  if(reply.job_ticket!=job||!bounded()||!npu_convolution_output(c,NPU_RUNTIME_OUTPUT,a733_npu_application_reference_bytes))goto failed;
  if(!request(c,NPU_SERVICE_REAP,NULL,0,&reply)||reply.state!=A733_NPU_READY||!bounded())goto failed;
  npu_runtime.job=job;++npu_runtime.jobs;npu_runtime.irq_count=npu_service_irq_completions(&c->service);
  if(npu_runtime.irq_count!=npu_runtime.jobs+1)goto failed;
  a733_observe_digest(NPU_RUNTIME_OUTPUT,a733_npu_application_reference_bytes,npu_runtime.output_digest);
  npu_runtime.end=a733_smp_counter();expected=NR_RUNNING;
  if(!bounded()||!atomic_compare_exchange_strong_explicit(&npu_runtime.state,&expected,NR_DONE,memory_order_release,memory_order_relaxed))goto failed;
 }
failed:
 npu_runtime_fail(40);vTaskSuspend(NULL);platform_panic("NPU runtime retained",40);
}
#ifdef SUNSTONE_ACCELERATORS
TaskHandle_t accelerator_npu_client_create(void)
{return xTaskCreateStaticAffinitySet(client,"npu-runtime",4096,NULL,2,client_stack,&client_state,1);}
#else
void vApplicationStackOverflowHook(TaskHandle_t task,char *name)
{(void)task;platform_panic(name,0);}
void a733_smp_diagnostic_start(void)
{
 configASSERT(a733_smp_device_preparation_admitted()&&npu_session_copy()&&a733_npu_application_observer_admit());
 TaskHandle_t task=xTaskCreateStaticAffinitySet(client,"npu-runtime",4096,NULL,2,client_stack,&client_state,1);
 configASSERT(task&&a733_npu_application_init(task));a733_observe_service_start();
 vTaskStartScheduler();platform_panic("scheduler returned",0);
}

#endif
