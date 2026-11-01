/* Compile this translation unit alone: inject failure at the application's
 * publication boundary, after it has observed the prior state. */
#include <assert.h>
static int inject;
static void before_transition(void);
#define SS_NPU_BEFORE_TRANSITION() before_transition()
#include "../../runtime/npu_app.c"
static void before_transition(void){if(inject){inject=0;ss_npu_service_fail();}}
static int run(unsigned m,const void *i,size_t ni,void *o,size_t no,uint64_t t)
{(void)m;(void)i;(void)ni;(void)o;(void)no;(void)t;return 1;}
static int close_backend(void){return 1;}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned mode=(unsigned)(argv[1][0]-'0');char in=1,out=0;
    const struct ss_npu_model model={"copy",1,1,1,1};
    struct ss_npu_backend backend={run,close_backend,&model,1};assert(ss_npu_service_bind(&backend));
    if(mode==0) {inject=1;assert(ss_npu_run(0,&in,1,&out,1,1)==-1);}
    if(mode==1) {
        assert(ss_npu_run(0,&in,1,&out,1,1)==0);assert(ss_npu_service_step()==1);
        inject=1;assert(ss_npu_run(0,&in,1,&out,1,1)==-1);
    }
    if(mode==2) {inject=1;assert(ss_npu_close()==-1);}
    if(mode==3) {
        assert(ss_npu_close()==0);assert(ss_npu_service_step()==1);
        inject=1;assert(ss_npu_close()==-1);
    }
    assert(atomic_load(&box.state)==FAILED);
    assert(ss_npu_run(0,&in,1,&out,1,1)==-1);
    assert(ss_npu_close()==-1&&ss_npu_service_step()==-1);
}
