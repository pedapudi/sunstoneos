#include "sunstone_app.h"
#include "sunstone_npu_service.h"
#include <assert.h>
#include <string.h>
static unsigned calls,closed;
static int fail,refuse;
static int run(unsigned model,const void *in,size_t ni,void *out,size_t no,uint64_t timeout)
{
    ++calls;assert(model==1&&ni==4&&no==4&&timeout==100);
    if(fail)return -1;
    if(refuse)return 0;
    memcpy(out,in,4);return 1;
}
static int close_backend(void){++closed;return 1;}
int main(int argc,char **argv)
{
    char in[4]="abc",out[4]={0};
    assert(ss_npu_run(1,in,4,out,4,100)==-1);
    const struct ss_npu_backend backend={run,close_backend};
    assert(ss_npu_service_bind(&backend));assert(!ss_npu_service_bind(&backend));
    assert(ss_npu_run(1,in,4,out,4,100)==0);
    assert(ss_npu_close()==-1);assert(ss_npu_run(2,in,4,out,4,100)==-1);
    assert(calls==0&&out[0]==0);fail=argc>1&&argv[1][0]!='r';refuse=argc>1&&argv[1][0]=='r';
    if(fail) {
        assert(ss_npu_service_step()==-1);
        assert(ss_npu_run(1,in,4,out,4,100)==-1);
        assert(ss_npu_close()==-1&&ss_npu_service_step()==-1);
        assert(calls==1&&!closed&&!out[0]);return 0;
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
