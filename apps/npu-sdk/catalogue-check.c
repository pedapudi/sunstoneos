/* Host-only: decode the generated linked models, then exercise the real OS
 * metadata admission and the public mailbox without starting a device. */
#include "npu.h"
#include "sunstone_npu_service.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
/* Newlib's assertion symbol is absent from host libc. */
_Noreturn void __assert_func(const char *file,int line,const char *function,const char *condition)
{(void)file;(void)line;(void)function;(void)condition;abort();}
extern int os_npu_sdk_bind(const struct npu_runtime *);
/* A refused geometry must never reach a device operation. */
int os_npu_load(unsigned entry){(void)entry;abort();}
int os_npu_tensor(enum nbg_section_id section,unsigned index,struct npu_runtime_tensor *out)
{(void)section;(void)index;(void)out;abort();}
int os_npu_submit(uint64_t timeout){(void)timeout;abort();}
int os_npu_close(void){abort();}
int main(int argc,char **argv)
{
    assert(argc==3);
    const unsigned expected=(unsigned)strtoul(argv[1],NULL,10);
    const unsigned fault=(unsigned)strtoul(argv[2],NULL,10);
    static struct npu_runtime runtime;
    runtime.count=os_npu_model_count;
    for(unsigned i=0;i<runtime.count;++i) {
        struct nbg_model model;
        assert(nbg_model_open(os_npu_models[i].data,os_npu_models[i].bytes,&model)==NBG_DECODE_OK);
        assert(nbg_assembly_layout(&model,&runtime.plans[i]));
    }
    if(fault==1)++runtime.plans[os_npu_client_entry].inputs;
    if(fault==2)++runtime.plans[os_npu_client_entry].buffers[NBG_ALLOC_INPUT].payload_bytes;
    if(fault) {
        assert(!os_npu_sdk_bind(&runtime));
        assert(!ss_npu_model_count());return 0;
    }
    assert(os_npu_sdk_bind(&runtime));
    assert(ss_npu_model_count()==expected);
    unsigned selected=UINT32_MAX;
    assert(ss_npu_model_find("shufflenet-v2-published",&selected));
    assert(selected==os_npu_client_entry);
    assert(!ss_npu_model_find("absent-model",&selected));
    struct ss_npu_model info;
    assert(ss_npu_model_info(selected,&info));
    assert(!strcmp(info.name,"shufflenet-v2-published"));
    assert(info.inputs==os_npu_input_count&&info.outputs==os_npu_output_count);
    size_t inputs=0,outputs=0;
    for(unsigned i=0;i<os_npu_input_count;++i)inputs+=os_npu_inputs[i].bytes;
    for(unsigned i=0;i<os_npu_output_count;++i)outputs+=os_npu_references[i].bytes;
    assert(info.input_bytes==inputs&&info.output_bytes==outputs);
    assert(!ss_npu_model_info(expected,&info));
    assert(!ss_npu_model_find(NULL,&selected)&&!ss_npu_model_find("shufflenet-v2-published",NULL));
    static unsigned char input[SS_NPU_MAX_TENSOR_BYTES],output[SS_NPU_MAX_TENSOR_BYTES];
    assert(ss_npu_run(expected,input,inputs,output,outputs,100)==-2);
    assert(ss_npu_run(selected,input,inputs-1,output,outputs,100)==-2);
    assert(ss_npu_run(selected,input,inputs,output,outputs+1,100)==-2);
    assert(ss_npu_service_step()==0);
    assert(ss_npu_run(selected,input,inputs,output,outputs,100)==0);
    /* Mismatched polling cannot replace or cancel the accepted request. */
    assert(ss_npu_run(expected,input,inputs,output,outputs,100)==-1);
    return 0;
}
