/* Actual SDK adapter and owner loop; runtime and RTOS effects are modeled. */
#include "../apps/os/npu-client.c"
#include "../runtime/npu_app.c"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
struct board_application board;
const struct ss_application ss_application={.manifest=(const void *)1};
const unsigned os_npu_model_count=1,os_npu_client_entry=0,os_npu_client_model=0,os_npu_input_count=0,os_npu_output_count=0;
const struct os_npu_vector os_npu_inputs[1]={{0}},os_npu_references[1]={{0}};
const struct ss_npu_model os_npu_model_info[]={{"copy",1,1,4,4}};
static struct os_session session;
static char input[4]="abc",output[4],tensor_input[4],tensor_output[4];
static unsigned loads,submits,ready,closed,waits,progresses,notifications;
static jmp_buf ended;
const struct os_session *os_session(void){return &session;}
int os_npu_progress(void){++progresses;return 1;}
int os_npu_open(void){return 1;}
int os_npu_stopping(void){return 0;}
unsigned os_npu_stopped(void){return closed;}
int os_npu_load(unsigned model){assert(!model);++loads;ready=1;return 1;}
int os_npu_tensor(enum nbg_section_id kind,unsigned index,struct npu_runtime_tensor *t)
{
    assert(kind==NBG_INPUT||kind==NBG_OUTPUT);
    if(!ready||index)return 0;
    *t=(struct npu_runtime_tensor){.data=kind==NBG_INPUT?tensor_input:tensor_output,.bytes=4};
    return 1;
}
struct os_npu_fault_info os_npu_last_fault;
int os_npu_submit(uint64_t timeout)
{assert(timeout==100);++submits;memcpy(tensor_output,tensor_input,4);return 1;}
int os_npu_close(void){closed=1;return 1;}
int board_live(void *v){assert(!v);return 1;}
void board_fail_note(uint32_t s,uint32_t v,const void *p){(void)s;(void)v;(void)p;assert(0);}
void board_fail(void *v){(void)v;assert(0);}
void vTaskSuspend(TaskHandle_t task){assert(!task);longjmp(ended,1);}
static void notify(void *unused){(void)unused;++notifications;}
uint32_t ulTaskGenericNotifyTake(UBaseType_t index,BaseType_t clear,TickType_t wait)
{
    assert(!index&&clear==pdTRUE&&wait==portMAX_DELAY);
    ++waits;
    if(waits==1)return 1; /* Thermal health update while idle. */
    if(waits==2)assert(ss_npu_run(0,input,4,output,4,100)==0);
    else if(waits==3) {
        assert(ss_npu_run(0,input,4,output,4,100)==1&&!memcmp(input,output,4));
        assert(ss_npu_close()==0);
    } else assert(0);
    return 1;
}
void storage_digest_init(struct storage_digest *s){(void)s;assert(0);}
int storage_digest_add(struct storage_digest *s,const void *data,size_t bytes)
{(void)s;(void)data;(void)bytes;assert(0);return 0;}
void storage_digest_end(const struct storage_digest *s,uint8_t out[32])
{(void)s;(void)out;assert(0);}
int main(void)
{
    /* First use loads; another use of the same READY model reuses it. */
    assert(sdk_run(0,input,4,output,4,100)==1&&loads==1&&submits==1);
    assert(sdk_run(0,input,4,output,4,100)==1&&loads==1&&submits==2);
    /* A lost READY window requires a new LOAD even for the same model. */
    ready=0;assert(sdk_run(0,input,4,output,4,100)==1&&loads==2&&submits==3);
    assert(sdk_run(os_npu_model_count,input,4,output,4,100)==0&&loads==2&&submits==3);
    assert(sdk_run(0,input,3,output,4,100)==0&&loads==2&&submits==3);
    struct npu_runtime runtime={.count=1};
    struct nbg_assembly_layout *plan=&runtime.plans[0];
    plan->inputs=1;plan->outputs=1;
    plan->buffers[NBG_ALLOC_INPUT].payload_bytes=4;
    plan->buffers[NBG_ALLOC_INPUT+1].payload_bytes=4;
    assert(ss_npu_service_bind_event(notify,NULL)&&os_npu_sdk_bind(&runtime));
    if(!setjmp(ended))os_npu_client_task(NULL);
    assert(waits==3&&progresses==5&&notifications==2&&loads==2&&submits==4);
    assert(ss_npu_close()==1&&os_npu_complete());
    puts("PASS SDK READY-model reuse and owner indefinite wait, health wake, request wake and close");
}
