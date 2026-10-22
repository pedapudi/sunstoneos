/* Host model of mimus linear-layer placement in the integrated OS image
 * (docs/recognition-dispatch.md). It compiles apps/os/dispatch.c with a
 * generated placement table and runs a call list through a modeled GPU hook
 * and GPU worker. The hook follows the GPU kernel design: jobs of at most
 * job_rows rows on 32-row boundaries, two jobs for a call of more than 32
 * rows except a subsampling block, the CPU computing channels [n_gpu, N), a
 * refused job computed on the CPU, and RESET or LOST reported through
 * os_dispatch_failed before any CPU row. apps/os/services.c supervises the
 * recognizer while the hook waits, and a modeled EL0 translation checks the
 * sum slots' mapping rule. No hardware is modeled.
 *
 *   clang -std=c11 -O2 -Iinclude -Iplatform/a733-smp-cached -Iapps/os -IOUT \
 *     -DSUNSTONE_BOARD_SUPERVISED=1 -DSUNSTONE_BOARD_USB2_DUAL=1 -DSUNSTONE_BOARD_RESIDENT=1 \
 *     -DSUNSTONE_A733_OS_GPU_LINEAR=1 -DOS_RECOGNITION_CORE=2 \
 *     checks/os-image-dispatch.c -o dispatch && ./dispatch CALLS [--numeric]
 *
 * OUT holds placement.h from apps/os/placement.py; CALLS is its --calls
 * output. The program prints one JSON line of counts. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The image's dispatcher, compiled into this file so that each case can
 * start from a closed session. */
#include "../apps/os/dispatch.c"

/* Modeled counter and WDT0 for the resident supervision in services.c. */
static uint64_t ticks;
uint64_t a733_smp_counter(void){return ticks;}
#define HZ UINT64_C(24000000)
static uint64_t expires,reloads,last_reload;
int os_watchdog_reload(void *supervisor)
{
    (void)supervisor;
    if(ticks>=expires)return 0;
    expires=ticks+4*HZ;++reloads;last_reload=ticks;return 1;
}
#include "../apps/os/services.c"

/* Recognizer side: os_dispatch_stop takes the recognizer's failure path,
 * which calls board_fail and suspends; here it returns to the case. */
static jmp_buf stop;
static unsigned board_fails;
void os_dispatch_stop(void){++board_fails;longjmp(stop,1);}

/* Modeled GPU worker. open_reply is OPEN's reply; script makes job `job` of
 * call `call` end in `status`. */
enum { JOB_OK, JOB_REFUSED, JOB_RESET, JOB_LOST };
static int open_reply=1;
static unsigned opens;
static struct { unsigned call,job,status; } script={~0u,~0u,JOB_OK};
int a733_gpu_mimus_open(void){++opens;return open_reply;}
void a733_gpu_mimus_worker(void *context){(void)context;}
int a733_gpu_mimus_start(unsigned core){return core==OS_RECOGNITION_CORE;}
void a733_gpu_mimus_irq(void){}

/* Modeled EL0 translation of the sum slots: every page EL0-readable as
 * inner-shareable write-back at its own address and not EL0-writable, except
 * the page faults that a case selects. */
static uintptr_t writable_page=~(uintptr_t)0,unreadable_page=~(uintptr_t)0,device_page=~(uintptr_t)0;
static unsigned translations;
static int translate_el0(uintptr_t address,int write,struct aarch64_translation *t)
{
    ++translations;
    uintptr_t page=address&~(uintptr_t)4095;
    if(write?page!=writable_page:page==unreadable_page)return 0;
    *t=(struct aarch64_translation){address,page==device_page?A733_MAIR_DEVICE_NGNRNE:A733_MAIR_NORMAL_WB,3};
    return 1;
}
static void slot_mapping_cases(void)
{
    translations=0;
    assert(os_dispatch_el0_read_only(translate_el0,OS_DISPATCH_SLOTS,OS_DISPATCH_SLOT_BYTES)&&translations==2048);
    /* One writable, unreadable or Device page among the 1,024 refuses. */
    uintptr_t last=OS_DISPATCH_SLOTS+OS_DISPATCH_SLOT_BYTES-4096,middle=OS_DISPATCH_SLOTS+0x200000;
    uintptr_t *faults[3]={&writable_page,&unreadable_page,&device_page};
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j){
        *faults[i]=j==0?OS_DISPATCH_SLOTS:j==1?middle:last;
        assert(!os_dispatch_el0_read_only(translate_el0,OS_DISPATCH_SLOTS,OS_DISPATCH_SLOT_BYTES));
        *faults[i]=~(uintptr_t)0;
    }
    assert(!os_dispatch_el0_read_only(translate_el0,OS_DISPATCH_SLOTS+64,4096)&&
        !os_dispatch_el0_read_only(translate_el0,OS_DISPATCH_SLOTS,100)&&
        !os_dispatch_el0_read_only(translate_el0,OS_DISPATCH_SLOTS,0)&&
        !os_dispatch_el0_read_only(NULL,OS_DISPATCH_SLOTS,4096));
}

struct call { uint32_t ordinal,rows,k,n,residual,block; };
static struct call *calls;
static unsigned call_count;

/* Counts over one run of the call list. */
static struct {
    uint64_t placed,jobs,block_jobs,block_padded,block_codes,block_lines,cpu_fallback_rows,
        outputs,written,mismatched,max_jobs_per_call,zero_row_jobs,place_lookups;
} count;

/* Codes and outputs of the current call. */
static int16_t *x;
static int8_t *w;
static int64_t *y,*reference;
static uint8_t *writes;
static int numeric;
static uint64_t seed=0x9e3779b97f4a7c15u;
static uint64_t next(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}

/* Exact sum of one output: chunks of at most 512 products fit int32 because
 * |x| <= 32767 and |w| <= 128 (GPU kernel design). */
static int64_t dot(const struct call *c,uint32_t row,uint32_t channel)
{
    const int16_t *a=x+(size_t)row*c->k;const int8_t *b=w+(size_t)channel*c->k;
    int64_t sum=0;
    for(uint32_t base=0;base<c->k;base+=512){
        uint32_t end=base+512<c->k?base+512:c->k;int32_t chunk=0;
        for(uint32_t i=base;i<end;++i)chunk+=(int32_t)a[i]*b[i];
        sum+=chunk;
    }
    return sum;
}
/* The read-out: stores the sum, or adds it once into the residual stream. */
static void output(const struct call *c,uint32_t row,uint32_t channel,int64_t sum)
{
    size_t i=(size_t)row*c->n+channel;
    ++writes[i];++count.written;
    if(numeric)y[i]=c->residual?y[i]+sum:sum;
}
static void cpu_rows(const struct call *c,uint32_t first,uint32_t rows,uint32_t c0,uint32_t c1)
{
    for(uint32_t r=first;r<first+rows;++r)for(uint32_t ch=c0;ch<c1;++ch)output(c,r,ch,numeric?dot(c,r,ch):0);
}

/* Jobs of one call, as apps/os/placement.py split() issues them. */
static unsigned split(const struct call *c,uint32_t job_rows,uint32_t *jobs)
{
    unsigned parts=(c->rows+job_rows-1)/job_rows;
    if(parts==1&&c->rows>32&&!c->block)parts=2;
    uint32_t left=c->rows;
    for(unsigned i=parts,j=0;i>0;--i,++j){
        uint32_t take=left;
        if(i>1){
            double half=(double)left/i/32;uint32_t t=32*(uint32_t)(half+0.5);
            /* Python's round() rounds halves to even. */
            if(half-(uint32_t)half==0.5&&((uint32_t)half&1)==0)t-=32;
            if(t<32)t=32;
            take=t<job_rows?t:job_rows;
        }
        jobs[j]=take;left-=take;
    }
    return parts;
}

/* The GPU hook for one call. */
static void hook(unsigned index,const struct call *c)
{
    struct os_placement p=os_dispatch_place(c->ordinal);++count.place_lookups;
    if(!p.n_gpu){cpu_rows(c,0,c->rows,0,c->n);return;}
    assert(p.n_gpu<=c->n&&p.job_rows>=32&&p.job_rows%32==0);
    unsigned width=c->k<=512?4:8;
    assert((uint64_t)p.job_rows*c->k*p.n_gpu<=(UINT64_C(1)<<26)&&(uint64_t)p.job_rows*p.n_gpu*width<=(2u<<20));
    os_dispatch_placed();++count.placed;
    /* Both inference workers compute the CPU channels while the GPU runs. */
    cpu_rows(c,0,c->rows,p.n_gpu,c->n);
    uint32_t jobs[64];unsigned parts=split(c,p.job_rows,jobs);
    assert(parts<=64);
    if(parts>count.max_jobs_per_call)count.max_jobs_per_call=parts;
    for(unsigned j=0,first=0;j<parts;first+=jobs[j++]){
        uint32_t r=jobs[j];
        if(!r)++count.zero_row_jobs;
        assert(r&&r<=p.job_rows&&(j+1==parts||r%32==0));
        ++count.jobs;
        if(c->block){
            ++count.block_jobs;count.block_padded+=(32-r%32)%32;
            count.block_lines+=((uint64_t)r*2*c->k+63)/64;
        }
        unsigned status=index==script.call&&j==script.job?script.status:JOB_OK;
        if(status==JOB_REFUSED){
            /* Refused before device access: the CPU computes those rows. */
            os_dispatch_refused();cpu_rows(c,first,r,0,p.n_gpu);count.cpu_fallback_rows+=r;continue;
        }
        if(status==JOB_RESET||status==JOB_LOST)os_dispatch_failed(status==JOB_RESET?OS_DISPATCH_RESET:OS_DISPATCH_LOST);
        /* The worker's exact sums land in a slot; the hook reads them out. */
        for(uint32_t row=first;row<first+r;++row)for(uint32_t ch=0;ch<p.n_gpu;++ch){
            int64_t sum=numeric?dot(c,row,ch):0;
            if(width==4)assert(sum>=INT32_MIN&&sum<=INT32_MAX);
            output(c,row,ch,sum);
        }
    }
}

/* Runs the call list with fresh codes per call and checks every output
 * against a CPU-only reference. Returns 1, or 0 when a GPU fault ended it. */
static void release(void)
{
    free(writes);free(y);free(reference);free(x);free(w);
    writes=NULL;y=reference=NULL;x=NULL;w=NULL;
}
static int run(void)
{
    memset(&count,0,sizeof(count));seed=0x9e3779b97f4a7c15u;
    if(setjmp(stop)){release();return 0;}
    for(unsigned i=0;i<call_count;++i){
        const struct call *c=&calls[i];
        size_t outputs=(size_t)c->rows*c->n;
        writes=calloc(outputs,1);y=calloc(outputs,8);reference=calloc(outputs,8);
        assert(writes&&y&&reference);
        if(c->block)count.block_codes+=(uint64_t)c->rows*c->k;
        if(numeric){
            x=malloc((size_t)c->rows*c->k*2);w=malloc((size_t)c->n*c->k);assert(x&&w);
            /* Row 0 and channel 0 hold the extreme codes, the rest random ones. */
            for(size_t j=0;j<(size_t)c->rows*c->k;++j)x[j]=j<c->k?32767:(int16_t)(next()%65535-32767);
            for(size_t j=0;j<(size_t)c->n*c->k;++j)w[j]=j<c->k?-128:(int8_t)(next()%256-128);
            for(size_t j=0;j<outputs;++j)y[j]=reference[j]=c->residual?(int64_t)(next()%1000):0;
            for(uint32_t r=0;r<c->rows;++r)for(uint32_t ch=0;ch<c->n;++ch)
                reference[(size_t)r*c->n+ch]+=dot(c,r,ch);
        }
        hook(i,c);
        for(size_t j=0;j<outputs;++j){
            assert(writes[j]==1);
            if(numeric&&y[j]!=reference[j])++count.mismatched;
        }
        count.outputs+=outputs;
        release();
    }
    return 1;
}
static void reset(void)
{
    memset(&os_dispatch_record,0,sizeof(os_dispatch_record));os_dispatch_active=0;
    opens=0;board_fails=0;open_reply=1;script.call=script.job=~0u;script.status=JOB_OK;
}

/* Supervision: every other service reports each millisecond; the recognizer
 * reports until the hook starts to wait at 1 s and again when the wait ends
 * after wait_ms (never for 0). The supervisor runs every 20 ms. Returns the
 * modeled watchdog's expiry in milliseconds, or 0 if it renewed throughout. */
static uint64_t supervise(uint64_t wait_ms,uint64_t end_ms,uint64_t *renewed_ms)
{
    memset(seen,0,sizeof(seen));ticks=0;expires=4*HZ;reloads=0;last_reload=0;
    int parked=0;
    for(uint64_t ms=0;ms<end_ms;++ms){
        ticks=ms*(HZ/1000);
        if(ticks>=expires){*renewed_ms=last_reload/(HZ/1000);return ms;}
        for(unsigned s=0;s<OS_SERVICES;++s){
            int waiting=s==OS_SERVICE_MIMUS&&ms>=1000&&(!wait_ms||ms<1000+wait_ms);
            if(!waiting)board_service_progress(s);
        }
        if(!parked&&ms%20==0&&board_resident_renew(&ticks,4*HZ)!=1)parked=1;
    }
    *renewed_ms=last_reload/(HZ/1000);return 0;
}

int main(int argc,char **argv)
{
    assert(argc>=2);
    numeric=argc>2&&!strcmp(argv[2],"--numeric");
    FILE *f=fopen(argv[1],"r");assert(f);
    struct call c;unsigned capacity=0;
    while(fscanf(f,"%u %u %u %u %u %u",&c.ordinal,&c.rows,&c.k,&c.n,&c.residual,&c.block)==6){
        if(call_count==capacity){capacity=capacity?2*capacity:256;calls=realloc(calls,capacity*sizeof(*calls));assert(calls);}
        assert(c.ordinal<OS_PLACEMENT_TENSORS&&c.rows&&c.k&&c.n);
        calls[call_count++]=c;
    }
    fclose(f);assert(call_count);

    slot_mapping_cases();

    /* No OPEN before the recognizer has matched every model hash: until then
     * every row reads as CPU-only and nothing reaches the worker. */
    reset();
    for(uint32_t i=0;i<OS_PLACEMENT_TENSORS;++i)assert(!os_dispatch_place(i).n_gpu);
    assert(run()&&!count.placed&&!opens&&!count.mismatched);

    /* A refused OPEN keeps the CPU for the boot and is not retried. */
    reset();open_reply=0;
    assert(!os_dispatch_open()&&!os_dispatch_open()&&opens==1&&os_dispatch_record.open_failed);
    assert(run()&&!count.placed&&!count.mismatched&&!board_fails);

    /* Nominal: OPEN once, then every placed call on the GPU. */
    reset();assert(os_dispatch_open()&&os_dispatch_open()&&opens==1);
    assert(run()&&!count.mismatched&&!count.zero_row_jobs&&!board_fails);
    assert(os_dispatch_record.placed==count.placed&&!os_dispatch_record.refused);
    __typeof__(count) nominal=count;

    /* A job refused before device access runs on the CPU; the session stays
     * open and every later call is still placed. */
    unsigned refused_call=0;
    while(refused_call<call_count&&!os_placement_table[calls[refused_call].ordinal].n_gpu)++refused_call;
    assert(refused_call<call_count);
    reset();assert(os_dispatch_open());script.call=refused_call;script.job=0;script.status=JOB_REFUSED;
    assert(run()&&!count.mismatched&&os_dispatch_record.refused==1&&count.cpu_fallback_rows>0);
    assert(os_dispatch_record.placed==nominal.placed&&!board_fails&&!os_dispatch_record.failure);

    /* RESET and LOST end the boot before any CPU row of the failed job, and
     * nothing is placed afterwards. The fault hits the last call's last job. */
    unsigned last=call_count;
    while(last&&!os_placement_table[calls[last-1].ordinal].n_gpu)--last;
    assert(last);
    for(unsigned status=JOB_RESET;status<=JOB_LOST;++status){
        reset();assert(os_dispatch_open());
        script.call=last-1;script.status=status;
        uint32_t jobs[64];struct os_placement p=os_placement_table[calls[last-1].ordinal];
        script.job=split(&calls[last-1],p.job_rows,jobs)-1;
        assert(!run());
        assert(board_fails==1&&!count.cpu_fallback_rows);
        assert(os_dispatch_record.failure==(status==JOB_RESET?OS_DISPATCH_RESET:OS_DISPATCH_LOST));
        for(uint32_t i=0;i<OS_PLACEMENT_TENSORS;++i)assert(!os_dispatch_place(i).n_gpu);
    }

    /* Supervision. A reply that never comes stops the recognizer's progress:
     * its 3 s window expires, renewal stops and the watchdog resets the board
     * within 4 s of the last renewal. Two consecutive jobs that each end at
     * the worker's deadline keep the window: the deadline is twice the
     * largest job's activation clean (0.45 ms), 100 ms per job, 4 ms of hang
     * detection, a 100 ms stop bound and 1 ms. */
    uint64_t renewed,expired=supervise(0,20000,&renewed);
    assert(expired&&renewed<=1000+3000&&expired-renewed<=4000&&expired>1000+3000);
    uint64_t answer_ms=1+100+4+100+1;
    assert(!supervise(2*answer_ms,20000,&renewed));

    printf("{\"calls\":%u,\"placed_calls\":%llu,\"jobs\":%llu,\"block_jobs\":%llu,\"block_padded_rows\":%llu,"
           "\"block_codes\":%llu,\"block_clean_lines\":%llu,\"outputs\":%llu,\"mismatched\":%llu,"
           "\"max_jobs_per_call\":%llu,\"zero_row_jobs\":%llu,\"lookups_per_call\":%.3f,"
           "\"watchdog_expiry_after_stall_ms\":%llu,\"slot_translations\":%u,\"numeric\":%d}\n",
           call_count,(unsigned long long)nominal.placed,(unsigned long long)nominal.jobs,
           (unsigned long long)nominal.block_jobs,(unsigned long long)nominal.block_padded,
           (unsigned long long)nominal.block_codes,(unsigned long long)nominal.block_lines,
           (unsigned long long)nominal.outputs,(unsigned long long)nominal.mismatched,
           (unsigned long long)nominal.max_jobs_per_call,(unsigned long long)nominal.zero_row_jobs,
           (double)nominal.place_lookups/call_count,(unsigned long long)(expired-1000),2048u,numeric);
    return 0;
}
