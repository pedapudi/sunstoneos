/* Run the actual OS SDK owner through refusal, successful copies and CLOSE;
 * the final immutable report must count only copied successful results. */
#include "../apps/os/npu-client.c"
#include "../apps/os/quiesce.c"
#include "npu-queue-unlinked.c"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
/* The client runs outside supervisor return. Route its fault callback through
 * the real registry with an unrequested owner descriptor. */
static struct os_quiesce report_quiesce={.owner=OS_QUIESCE_NPU,.order=OS_QUIESCE_QUEUES};
int os_cpu_return_requested(void){return 0;}
int os_cpu_return_failing(void){return 0;}
int os_npu_quiesce_fault(uint32_t site,uint32_t value,const void *caller)
{return os_quiesce_fault(&report_quiesce,site,value,caller);}
struct board_application board;
struct board_provider providers;
static struct ss_manifest manifest;
#ifdef REPORT_BUILTIN
const struct ss_application ss_application={0};
#else
const struct ss_application ss_application={.manifest=&manifest};
#endif
const unsigned os_npu_client_model=3;
static const uint8_t input[]={1,2,3,4};
static unsigned step,submits,failed,refuse_submit,after_submit,native_closed,mode,input_visits;
static uint32_t first_site,first_value;
static uint8_t tensor_input[4],tensor_output[8];
static struct os_session session;
static jmp_buf suspended;
_Noreturn void __assert_func(const char *file,int line,const char *function,const char *condition)
{(void)file;(void)line;(void)function;puts(condition);fflush(NULL);abort();}
const struct os_session *os_session(void){return &session;}
int os_npu_open(void){return 1;}
int os_npu_load(unsigned model){assert(!model);after_submit=0;input_visits=0;return 1;}
int os_npu_tensor(enum nbg_section_id section,unsigned index,struct npu_runtime_tensor *t)
{
    if(section==NBG_OUTPUT&&mode==5) {
        if(index>=2||(after_submit&&step==1&&index==1))return 0;
        t->data=tensor_output+4*index;t->bytes=4;return 1;
    }
    if(section==NBG_INPUT&&!index&&mode==4&&step==1&&input_visits++>=2)return 0;
    if(index||(section==NBG_OUTPUT&&after_submit&&((mode==1&&step==1)||(mode==2&&step==3))))return 0;
    t->data=section==NBG_INPUT?tensor_input:tensor_output;
    t->bytes=section==NBG_INPUT?sizeof(tensor_input):sizeof(tensor_output);return 1;
}
int os_npu_submit(uint64_t timeout)
{
    assert(timeout==100||timeout==1000000);++submits;if(refuse_submit)return 0;
    after_submit=1;
    assert(!memcmp(tensor_input,input,sizeof(input)));
    for(unsigned i=0;i<sizeof(tensor_output);++i)tensor_output[i]=(uint8_t)(i+1);
    return 1;
}
int os_npu_close(void){native_closed=1;return 1;}
int os_npu_progress(void){return 1;}
int os_npu_stopping(void){return 0;}
unsigned os_npu_stopped(void){return native_closed;}
int board_live(void *context){assert(!context);return 1;}
/* A supervised session: the lifetime the NPU service reads (apps/os/lifetime.c, apps/os/sealed.c); it reports no
 * resident progress. */
unsigned board_session_lifetime(void){return A733_LIFETIME_SUPERVISED;}
unsigned os_lifetime(void){return A733_LIFETIME_SUPERVISED;}
void board_service_progress(unsigned service){(void)service;}
void os_application_wake_main(void){}   /* the application's end wait (apps/os/application.c); no application runs here */
void board_fail_note(uint32_t site,uint32_t value,const void *caller)
{(void)caller;if(!first_site){first_site=site;first_value=value;}}
void board_fail(void *context)
{assert(!context);board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(BOARD),0,NULL);++failed;}
void ss_npu_service_fail(void){++failed;}
int ss_npu_service_step(void)
{
    uint8_t output[8]={0};int result;
    /* READY-model reuse probes tensor zero before the size and copy walks.
     * Each request begins with the preceding completed window readable. */
    after_submit=0;input_visits=0;
    if(step==0)result=sdk_run(0,input,sizeof(input)-1,output,sizeof(output),100);
    else if(step==4||(mode==3&&step==2))result=sdk_close();
    else {
        refuse_submit=step==2;
        result=sdk_run(0,input,sizeof(input),output,sizeof(output),100);
        if(result==1)assert(!memcmp(output,tensor_output,sizeof(output)));
    }
    const int partial=((mode==1||mode==4||mode==5)&&step==1)||(mode==2&&step==3);
    assert(result==(partial?-1:(step==0||(step==2&&mode!=3)?0:1)));
    ++step;return partial?-1:1;
}
uint32_t ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t wait)
{(void)index;(void)clear;(void)wait;return 0;}
void vTaskSuspend(TaskHandle_t task){assert(!task);longjmp(suspended,1);}
int main(int argc,char **argv)
{
    assert(argc==3);mode=(unsigned)strtoul(argv[1],NULL,10);
    board.session.epoch=10;board.session.generation=20;
    static struct npu_runtime runtime;atomic_init(&runtime.count,1);os_npu_runtime_for_sdk=&runtime;
    if(!setjmp(suspended))os_npu_client_task(NULL);
    struct os_npu_report report;os_npu_report_copy((uint8_t *)&report);
    assert(atomic_load(&report.finished)&&report.model==3);
#ifdef REPORT_BUILTIN
    /* An image without an application manifest has no client: the service opens and closes, and the report says refused. */
    assert(!mode&&!submits&&!failed&&os_npu_complete());
    assert(report.stopped&&!report.retained&&report.refused&&!report.jobs&&!report.output_bytes&&!report.mismatches);
    assert(!report.jobs_selected);   /* the session sets no job count, and no application ran */
#else
    const unsigned jobs=mode==0?2:mode==1||mode==4||mode==5?0:1;
    /* An application's jobs are not selected by the session: the report gives the jobs it ran in both words. */
    assert(report.jobs==jobs&&report.jobs_selected==jobs&&report.output_bytes==(jobs?8:0));
    if(mode==0)assert(step==5&&submits==3&&!failed&&os_npu_complete());
    if(mode==3)assert(step==3&&submits==1&&!failed&&os_npu_complete());
    if(mode==1||mode==2||mode==4||mode==5) {
        assert(failed&&report.retained&&!report.stopped&&!os_npu_complete());
        assert((first_site>>12&15)==SUNSTONE_CRUMB_FAIL_NPU&&!(first_site>>16)&&first_value==(mode==5?4u:0u));
    }
    else assert(report.stopped&&!report.retained&&!report.refused);
    assert(!report.pattern&&!report.pattern_bytes&&!report.mismatches);
    uint8_t zeros[32]={0};assert(!memcmp(report.last_digest,zeros,32));
#endif
    printf("%u %u\n",first_site,first_value);
    FILE *file=fopen(argv[2],"wb");assert(file);
    assert(fwrite(&report,1,sizeof(report),file)==sizeof(report)&&!fclose(file));
}
