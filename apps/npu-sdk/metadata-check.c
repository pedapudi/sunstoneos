/* Each process begins with an unbound service. Malformed immutable metadata
 * must refuse binding before publishing discovery or accepting a request. */
#include "sunstone_app.h"
#include "sunstone_npu_service.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static int run(unsigned n,const void *i,size_t ni,void *o,size_t no,uint64_t t)
{(void)n;(void)i;(void)ni;(void)o;(void)no;(void)t;abort();}
static int close_backend(void){abort();}
int main(int argc,char **argv)
{
    assert(argc==2);
    unsigned mode=(unsigned)strtoul(argv[1],NULL,10);
    struct ss_npu_model models[]={{"first",1,1,4,8},{"second",2,1,8,4}};
    struct ss_npu_backend backend={run,close_backend,models,2,NULL};
    if(mode==0)backend.models=NULL;
    if(mode==1)backend.count=0;
    if(mode==2)models[0].name[0]=0;
    if(mode==3)memset(models[0].name,'x',sizeof(models[0].name));
    if(mode==4)memcpy(models[1].name,models[0].name,sizeof(models[1].name));
    if(mode==5)models[0].inputs=0;
    if(mode==6)models[0].outputs=0;
    if(mode==7)models[0].input_bytes=0;
    if(mode==8)models[0].output_bytes=0;
    if(mode==9)models[0].input_bytes=SS_NPU_MAX_TENSOR_BYTES+1u;
    if(mode==10)models[0].output_bytes=SS_NPU_MAX_TENSOR_BYTES+1u;
    assert(!ss_npu_service_bind(&backend));
    struct ss_npu_model out;
    unsigned index=17;
    assert(!ss_npu_model_count()&&!ss_npu_model_info(0,&out));
    assert(!ss_npu_model_find("first",&index)&&index==17);
    assert(ss_npu_service_step()==0);
}
