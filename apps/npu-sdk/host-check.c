#include "sunstone_app.h"
#include "sunstone_npu_service.h"
#include <assert.h>
#include <string.h>
static unsigned calls,closed;
static int fail,refuse,jobfault,registering;
static unsigned registered;
static int register_graph(const void *g,size_t n,unsigned *index){(void)g;if(n==3)return 0;*index=2+registered++;return 1;}
static void describe(struct ss_npu_fault_info *f){f->mmu_status=2;f->mmu_address=0x20000000;f->canary_crc=7;f->marks=0x71f;f->window_ms=8000;}
static int run(unsigned model,const void *in,size_t ni,void *out,size_t no,uint64_t timeout)
{
    ++calls;assert((model==1||model>=2)&&ni==4&&no==4&&timeout==100);
    if(fail)return -1;
    if(refuse)return 0;
    if(jobfault&&calls==1)return 2;
    memcpy(out,in,4);return 1;
}
static int close_backend(void){++closed;return 1;}
int main(int argc,char **argv)
{
    char in[4]="abc",out[4]={0};
    assert(ss_npu_run(1,in,4,out,4,100)==-1);
    const struct ss_npu_model models[]={{"unused",1,1,1,1},{"copy",1,1,4,4}};
    const struct ss_npu_backend backend={run,close_backend,models,2,NULL};
    assert(ss_npu_service_bind_fault(describe)&&!ss_npu_service_bind_fault(describe));
    if(argc>1&&argv[1][0]=='g')assert(ss_npu_service_bind_register(register_graph)&&!ss_npu_service_bind_register(register_graph));
    assert(ss_npu_service_bind(&backend));assert(!ss_npu_service_bind(&backend));
    assert(ss_npu_run(1,in,4,out,4,100)==0);
    assert(ss_npu_close()==-1);assert(ss_npu_run(2,in,4,out,4,100)==-1);
    assert(calls==0&&out[0]==0);jobfault=argc>1&&argv[1][0]=='j';registering=argc>1&&argv[1][0]=='g';fail=argc>1&&argv[1][0]!='r'&&!jobfault&&!registering;refuse=argc>1&&argv[1][0]=='r';
    if(fail) {
        assert(ss_npu_service_step()==-1);
        assert(ss_npu_run(1,in,4,out,4,100)==-1);
        assert(ss_npu_close()==-1&&ss_npu_service_step()==-1);
        assert(calls==1&&!closed&&!out[0]);return 0;
    }
    if(argc>1&&argv[1][0]=='g'){
        /* A graph handed over at run time: registered through the mailbox, then run by the index it was given. */
        unsigned index=99;
        assert(ss_npu_service_step()==1&&ss_npu_run(1,in,4,out,4,100)==1); /* the request the common prefix left pending */
        assert(ss_npu_register_graph(in,4,&index)==0&&ss_npu_service_step()==1);
        assert(ss_npu_register_graph(in,4,&index)==1&&index==2);
        assert(ss_npu_register_graph(in,3,&index)==0&&ss_npu_service_step()==1&&ss_npu_register_graph(in,3,&index)==-2&&index==2);
        assert(ss_npu_run(3,in,4,out,4,100)==-2); /* nothing registered under 3 yet */
        assert(ss_npu_run(2,in,4,out,4,100)==0&&ss_npu_service_step()==1&&ss_npu_run(2,in,4,out,4,100)==1&&!memcmp(in,out,4));
        return 0;
    }
    if(jobfault) {
        /* An NPU MMU exception failed the job: -3, a description once, and the session goes on. */
        struct ss_npu_fault_info f;
        assert(ss_npu_service_step()==1&&calls==1);
        assert(ss_npu_run(1,in,4,out,4,100)==-3&&!out[0]);
        assert(ss_npu_last_fault(&f)==1&&f.mmu_status==2&&f.mmu_address==0x20000000&&f.canary_crc==7&&f.marks==0x71f&&f.window_ms==8000);
        assert(ss_npu_last_fault(&f)==0);
        assert(ss_npu_run(1,in,4,out,4,100)==0&&ss_npu_service_step()==1&&calls==2);
        assert(ss_npu_run(1,in,4,out,4,100)==1&&!memcmp(in,out,4));
        assert(ss_npu_close()==0&&ss_npu_service_step()==1&&ss_npu_close()==1);
        return 0;
    }
    assert(ss_npu_service_step()==1&&calls==1);
    assert(ss_npu_service_step()==0&&calls==1);
    if(refuse)assert(ss_npu_run(1,in,4,out,4,100)==-2&&!out[0]);
    else assert(ss_npu_run(1,in,4,out,4,100)==1&&!memcmp(in,out,4));
    assert(ss_npu_close()==0);assert(ss_npu_service_step()==1);
    assert(ss_npu_close()==1&&closed==1);
    assert(ss_npu_run(1,in,4,out,4,100)==-1);
    return 0;
}
