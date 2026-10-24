/* Host model of mimus linear-layer placement in the integrated OS image
 * (docs/recognition-dispatch.md). It compiles apps/os/dispatch.c with a
 * generated placement table and runs a call list through a modeled GPU hook
 * and GPU worker. The hook follows the GPU kernel design: one job in flight,
 * a call of more than job_rows rows split into consecutive jobs on 32-row
 * boundaries, the CPU computing channels [n_gpu, N), a refused job or a job
 * with a missing tag computed on the CPU, and RESET or LOST reported through
 * os_dispatch_failed before any CPU row. The modeled worker refuses a job
 * above its own bound, computed at OPEN with the kernel design's formula.
 * apps/os/services.c supervises the recognizer while the hook waits and
 * reports a GPU loss as its stop detail, and modeled EL1 and EL0
 * translations check the arena's and the output region's mapping rules at
 * both table strides. A replay of the image's pool admission order checks the
 * GPU's pages. No hardware is modeled.
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
/* The pool ledger and the GPU table planner, for the pool replay. */
#include "../runtime/device_memory_pool.c"
#include "../drivers/gpu/pvr_gpu_vm.c"
#include "pvr_compute_mimus.h"
/* Page counts the runner reads from the headers that include FreeRTOS:
 * a733_gpu_reserved.h, a733_gpu_mimus.h, pvr_mimus_service.h, a733_xhci.h. */
#if !defined(REPLAY_FIRMWARE_WB_PAGES) || !defined(REPLAY_FIRMWARE_NC_PAGES) || \
    !defined(REPLAY_SESSION_WB_PAGES) || !defined(REPLAY_SESSION_NC_PAGES) || !defined(REPLAY_XHCI_BYTES)
#error "the runner supplies the replayed allocation sizes"
#endif

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
enum { JOB_OK, JOB_REFUSED, JOB_RESET, JOB_LOST, JOB_INCOMPLETE };
static int open_reply=1;
static unsigned opens;
static struct { unsigned call,job,status; } script={~0u,~0u,JOB_OK};
int a733_gpu_mimus_open(void){++opens;return open_reply;}

/* Modeled EL1 and EL0 translations: every page inner-shareable write-back
 * at its own address; the arena EL1-writable, the read-only span
 * EL0-readable and writable at neither EL0 nor EL1, except the page faults
 * that a case selects. */
static uintptr_t writable_page=~(uintptr_t)0,el1_writable_page=~(uintptr_t)0,unreadable_page=~(uintptr_t)0,
    device_page=~(uintptr_t)0,arena_read_only_page=~(uintptr_t)0;
static unsigned translations;
static int translate_el0(uintptr_t address,int write,struct aarch64_translation *t)
{
    ++translations;
    uintptr_t page=address&~(uintptr_t)4095;
    if(write?page!=writable_page:page==unreadable_page)return 0;
    *t=(struct aarch64_translation){address,page==device_page?A733_MAIR_DEVICE_NGNRNE:A733_MAIR_NORMAL_WB,3};
    return 1;
}
static int translate_el1(uintptr_t address,int write,struct aarch64_translation *t)
{
    ++translations;
    uintptr_t page=address&~(uintptr_t)4095;
    int view=page>=OS_DISPATCH_READ_ONLY&&page<OS_DISPATCH_READ_ONLY+OS_DISPATCH_READ_ONLY_BYTES;
    if(write&&(page==arena_read_only_page||(view&&page!=el1_writable_page)))return 0;
    *t=(struct aarch64_translation){address,page==device_page?A733_MAIR_DEVICE_NGNRNE:A733_MAIR_NORMAL_WB,3};
    return 1;
}
/* Translations that mimus_board_pins makes for the arena and the view. */
static unsigned pins_translations;
static int view(uintptr_t stride)
{
    return os_dispatch_read_only(translate_el0,translate_el1,OS_DISPATCH_READ_ONLY,OS_DISPATCH_READ_ONLY_BYTES,stride);
}
static void mapping_cases(void)
{
    static const struct a733_mapping_rule writable={A733_MAPPING_WRITE,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    const uintptr_t block=(uintptr_t)2<<20,base=OS_DISPATCH_READ_ONLY,bytes=OS_DISPATCH_READ_ONLY_BYTES;
    /* A 4 KiB stride checks every page: 3,072 translations (EL0 read, EL0
     * write, EL1 write), and one EL0-writable, EL1-writable, unreadable or
     * Device page among the 1,024 refuses. */
    translations=0;
    assert(view(4096)&&translations==3072);
    uintptr_t last=base+bytes-4096,middle=base+0x200000+0x10000;
    uintptr_t *faults[4]={&writable_page,&el1_writable_page,&unreadable_page,&device_page};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<3;++j){
        *faults[i]=j==0?base:j==1?middle:last;
        assert(!view(4096));
        *faults[i]=~(uintptr_t)0;
    }
    /* The 2 MiB stride of platform/a733-smp-cached/memory.c: the first and
     * the last page of each block of the view, 12 translations, and one EL1
     * write translation per arena block, 6. A fault at either end of a view
     * block, or at the start of an arena block, refuses; one elsewhere cannot
     * exist while the QEMU golden comparison of
     * checks/device-memory-capacity.py keeps every entry a block descriptor. */
    translations=0;
    assert(view(block)&&translations==12);
    translations=0;
    assert(a733_mapping_check_stride(translate_el1,OS_DISPATCH_ARENA,OS_DISPATCH_ARENA,OS_DISPATCH_CODE_BYTES,block,
                                     &writable)&&translations==6);
    pins_translations=12+6;
    const uintptr_t ends[4]={base,base+block-4096,base+block,last};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j){
        *faults[i]=ends[j];
        assert(!view(block));
        *faults[i]=~(uintptr_t)0;
    }
    for(unsigned j=0;j<6;++j){
        uintptr_t start=OS_DISPATCH_ARENA+j*block;
        arena_read_only_page=start;
        assert(!a733_mapping_check_stride(translate_el1,OS_DISPATCH_ARENA,OS_DISPATCH_ARENA,OS_DISPATCH_CODE_BYTES,
                                          block,&writable));
        arena_read_only_page=~(uintptr_t)0;
    }
    /* Ranges and strides that do not fit the rule refuse. */
    assert(!os_dispatch_read_only(translate_el0,translate_el1,base+64,4096,4096)&&
        !os_dispatch_read_only(translate_el0,translate_el1,base,100,4096)&&
        !os_dispatch_read_only(translate_el0,translate_el1,base,0,4096)&&
        !os_dispatch_read_only(NULL,translate_el1,base,4096,4096)&&
        !os_dispatch_read_only(translate_el0,NULL,base,4096,4096)&&
        !os_dispatch_read_only(translate_el0,translate_el1,base+4096,block,block)&&
        !os_dispatch_read_only(translate_el0,translate_el1,base,block+4096,block)&&
        !os_dispatch_read_only(translate_el0,translate_el1,base,bytes,3*4096));
    /* The output region is the first 2 MiB of the span; mimus's arena ends
     * where the span begins. */
    assert(OS_DISPATCH_OUTPUT==OS_DISPATCH_READ_ONLY&&OS_DISPATCH_OUTPUT_BYTES<=OS_DISPATCH_READ_ONLY_BYTES&&
        OS_DISPATCH_ARENA+OS_DISPATCH_CODE_BYTES==OS_DISPATCH_READ_ONLY);
}

/* The image's pool admission order (apps/board/main.c, then the start list):
 * USB2's xHCI memory from the uncached pool, the microphone's DMA periods
 * from the write-back pool, then the GPU firmware and the mimus session.
 * The session's table pages come from the planner over the session's maps
 * (platform/a733/gpu_mimus.c). Returns the GPU's write-back and uncached
 * pages through the pointers. */
static unsigned char wb_backing[2<<20] __attribute__((aligned(4096))),nc_backing[2<<20] __attribute__((aligned(4096)));
static struct device_memory_pool wb_pool,nc_pool;
int aarch64_device_cache(enum aarch64_cache_op op,uintptr_t address,size_t bytes){(void)op;(void)address;(void)bytes;return 1;}
static unsigned session_table_pages(void)
{
    struct pvr_gpu_mapping maps[64];unsigned count=0;
    const uint64_t payload=0x41a00000,pds=UINT64_C(0xda00000000),usc=UINT64_C(0xe000000000),general=UINT64_C(0x8000000000);
    maps[count++]=(struct pvr_gpu_mapping){PVR_MIMUS_MODEL_GPU,OS_DISPATCH_MODELS,PVR_MIMUS_MODEL_BYTES,21,PVR_GPU_VM_READ_ONLY};
    maps[count++]=(struct pvr_gpu_mapping){PVR_MIMUS_MODEL_GPU+PVR_MIMUS_MODEL_BYTES,OS_DISPATCH_MODELS+OS_DISPATCH_MODEL_BYTES,
        PVR_MIMUS_MODEL_BYTES,21,PVR_GPU_VM_READ_ONLY};
    maps[count++]=(struct pvr_gpu_mapping){PVR_MIMUS_ARENA_GPU,OS_DISPATCH_ARENA,PVR_MIMUS_ARENA_BYTES,21,PVR_GPU_VM_READ_ONLY};
    maps[count++]=(struct pvr_gpu_mapping){PVR_MIMUS_OUTPUT_GPU,OS_DISPATCH_OUTPUT,PVR_MIMUS_OUTPUT_BYTES,21,0};
    static const uint32_t program_pages[10]={1,1,1,1,1,1,1,5,5,1};
    uint64_t physical=payload+3*4096;
    for(unsigned i=0;i<10;++i){
        uint64_t base=(i<2?usc:i<9?pds:general)+0x10000+i*0x50000u;
        for(unsigned k=0;k<program_pages[i];++k)
            maps[count++]=(struct pvr_gpu_mapping){base+k*4096u,physical+=4096,4096,12,i<7?PVR_GPU_VM_READ_ONLY:0};
    }
    maps[count++]=(struct pvr_gpu_mapping){pds+0x1e0000,payload,4096,12,PVR_GPU_VM_READ_ONLY};
    maps[count++]=(struct pvr_gpu_mapping){usc+0x100000,payload+4096,4096,12,PVR_GPU_VM_READ_ONLY};
    maps[count++]=(struct pvr_gpu_mapping){usc+0x101000,payload+8192,4096,12,PVR_GPU_VM_READ_ONLY};
    assert(count==4+REPLAY_SESSION_WB_PAGES);
    for(unsigned i=1;i<count;++i){struct pvr_gpu_mapping m=maps[i];unsigned j=i;
        while(j&&maps[j-1].address>m.address){maps[j]=maps[j-1];--j;}maps[j]=m;}
    struct pvr_gpu_vm_requirements r;
    assert(pvr_gpu_vm_require(maps,count,&r)&&r.table_pages);
    return r.table_pages;
}
static unsigned gpu_wb_pages,gpu_nc_pages,gpu_table_pages;
static void pool_replay(unsigned audio_pages)
{
    memset(&wb_pool,0,sizeof(wb_pool));memset(&nc_pool,0,sizeof(nc_pool));
    assert(device_memory_pool_init(&wb_pool,(uintptr_t)wb_backing,0x41a00000,sizeof(wb_backing))&&
           device_memory_pool_init(&nc_pool,(uintptr_t)nc_backing,0x41c00000,sizeof(nc_backing)));
    struct device_memory_lease lease;struct device_memory_extent extent;
    assert(device_memory_acquire(&nc_pool,1,REPLAY_XHCI_BYTES,&lease,&extent));
    if(audio_pages)assert(device_memory_acquire(&wb_pool,2,(size_t)audio_pages*4096,&lease,&extent));
    gpu_table_pages=session_table_pages();
    gpu_wb_pages=REPLAY_FIRMWARE_WB_PAGES+REPLAY_SESSION_WB_PAGES+gpu_table_pages;
    gpu_nc_pages=REPLAY_FIRMWARE_NC_PAGES+REPLAY_SESSION_NC_PAGES;
    assert(device_memory_acquire(&wb_pool,3,REPLAY_FIRMWARE_WB_PAGES*4096u,&lease,&extent)&&
           device_memory_acquire(&nc_pool,3,REPLAY_FIRMWARE_NC_PAGES*4096u,&lease,&extent)&&
           device_memory_acquire(&wb_pool,4,REPLAY_SESSION_WB_PAGES*4096u,&lease,&extent)&&
           device_memory_acquire(&nc_pool,4,REPLAY_SESSION_NC_PAGES*4096u,&lease,&extent)&&
           device_memory_acquire(&wb_pool,4,(size_t)gpu_table_pages*4096,&lease,&extent));
    assert(gpu_wb_pages<=80&&gpu_nc_pages<=52);
}

static void reset_state_for_detail(void);
/* GPU loss as the supervisor's stop detail (apps/os/services.c). */
static void stop_detail_cases(void)
{
    stop_detail=0;assert(!board_resident_stop_detail());
    /* A GPU fault: board_fail follows, and supervise() never renews again. */
    reset_state_for_detail();
    assert(os_dispatch_open());
    if(!setjmp(stop))os_dispatch_failed(OS_DISPATCH_LOST);
    assert(os_dispatch_gpu_state()==OS_DISPATCH_GPU_FAULT&&board_resident_stop_detail()==OS_STOP_DETAIL_GPU_LOSS);
    /* A silent recognizer while it waits for a job's record is a GPU loss;
     * outside that wait, or for another service's expiry, it is not. */
    reset_state_for_detail();assert(os_dispatch_open());
    stop_detail=OS_SERVICE_MIMUS+1;assert(board_resident_stop_detail()==OS_SERVICE_MIMUS+1);
    os_dispatch_waiting(1);
    assert(board_resident_stop_detail()==OS_STOP_DETAIL_GPU_LOSS);
    stop_detail=OS_SERVICE_THERMAL+1;assert(board_resident_stop_detail()==OS_SERVICE_THERMAL+1);
    os_dispatch_waiting(0);
    /* After a missing tag the session is idle and placement closed. */
    os_dispatch_incomplete();
    stop_detail=OS_SERVICE_MIMUS+1;assert(board_resident_stop_detail()==OS_SERVICE_MIMUS+1);
    /* A refused OPEN leaves no GPU state. */
    reset_state_for_detail();open_reply=0;assert(!os_dispatch_open());
    assert(board_resident_stop_detail()==OS_SERVICE_MIMUS+1);
    assert(OS_STOP_DETAIL_GPU_LOSS>OS_SERVICES);
    stop_detail=0;
}

struct call { uint32_t ordinal,rows,k,n,residual,block; };
static struct call *calls;
static unsigned call_count;

/* Counts over one run of the call list. */
static struct {
    uint64_t placed,jobs,block_jobs,block_padded,block_codes,block_lines,cpu_fallback_rows,
        outputs,written,mismatched,max_jobs_per_call,zero_row_jobs,place_lookups,worker_refusals;
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

/* Jobs of one call, as apps/os/placement.py split() issues them: job_rows
 * rows each and a last job with the rest. */
static unsigned split(const struct call *c,uint32_t job_rows,uint32_t *jobs)
{
    unsigned parts=0;
    for(uint32_t left=c->rows;left;left-=jobs[parts++])jobs[parts]=left<job_rows?left:job_rows;
    return parts;
}
/* The GPU worker's bound at OPEN (GPU kernel design): the 64 Mi MAC cap, the
 * 2 MiB output region less its 64 KiB tag area, and the 512 workgroups of
 * 32 rows by 16 channels whose lanes' tags fill the tag area. */
static uint64_t worker_job_rows(uint64_t k,uint64_t n_gpu)
{
    uint64_t width=k<=512?4:8,macs=(UINT64_C(1)<<26)/(k*n_gpu),
        sums=(OS_DISPATCH_OUTPUT_BYTES-(64u<<10))/(n_gpu*width),tags=32*(512/((n_gpu+15)/16));
    uint64_t rows=macs<sums?macs:sums;
    return rows<tags?rows:tags;
}

/* The GPU hook for one call. */
static void hook(unsigned index,const struct call *c)
{
    struct os_placement p=os_dispatch_place(c->ordinal);++count.place_lookups;
    if(!p.n_gpu){cpu_rows(c,0,c->rows,0,c->n);return;}
    assert(p.n_gpu<=c->n&&p.job_rows>=32&&p.job_rows%32==0);
    unsigned width=c->k<=512?4:8;
    uint64_t bound=worker_job_rows(c->k,p.n_gpu);
    assert(p.job_rows<=bound&&p.job_rows+32>bound);
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
        /* The worker refuses rows above its own bound; none may reach it. */
        if(r>bound)++count.worker_refusals;
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
        if(status==JOB_INCOMPLETE){
            /* A missing tag: after the worker's stop the session closes and
             * the CPU computes the rest of the call; later calls stay on the
             * CPU. */
            os_dispatch_incomplete();cpu_rows(c,first,c->rows-first,0,p.n_gpu);
            count.cpu_fallback_rows+=c->rows-first;break;
        }
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
    atomic_store(&gpu_state,OS_DISPATCH_GPU_IDLE);
    opens=0;board_fails=0;open_reply=1;script.call=script.job=~0u;script.status=JOB_OK;
}
static void reset_state_for_detail(void){reset();}

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

    mapping_cases();
    pool_replay(0);
    unsigned wb_without_audio=gpu_wb_pages;
    pool_replay(10);
    assert(gpu_wb_pages==wb_without_audio);
    stop_detail_cases();

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
    assert(run()&&!count.mismatched&&!count.zero_row_jobs&&!count.worker_refusals&&!board_fails);
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

    /* A job with a missing tag (or, in the validation build, a sum that
     * differs) runs on the CPU at once, and every later call stays on the CPU
     * without ending the boot. */
    reset();assert(os_dispatch_open());script.call=refused_call;script.job=0;script.status=JOB_INCOMPLETE;
    assert(run()&&!count.mismatched&&os_dispatch_record.incomplete==1&&count.placed==1&&!board_fails);
    for(uint32_t i=0;i<OS_PLACEMENT_TENSORS;++i)assert(!os_dispatch_place(i).n_gpu);

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
     * the worker's hang bound keep the window: per job at most 1 ms of
     * activation clean, max_job_us (1 s) from the kick and one 1 ms tick of
     * hang detection. */
    uint64_t renewed,expired=supervise(0,20000,&renewed);
    assert(expired&&renewed<=1000+3000&&expired-renewed<=4000&&expired>1000+3000);
    uint64_t answer_ms=1+1000+1;
    assert(!supervise(2*answer_ms,20000,&renewed));

    printf("{\"calls\":%u,\"placed_calls\":%llu,\"jobs\":%llu,\"block_jobs\":%llu,\"block_padded_rows\":%llu,"
           "\"block_codes\":%llu,\"block_clean_lines\":%llu,\"outputs\":%llu,\"mismatched\":%llu,"
           "\"max_jobs_per_call\":%llu,\"zero_row_jobs\":%llu,\"lookups_per_call\":%.3f,"
           "\"watchdog_expiry_after_stall_ms\":%llu,\"pins_translations\":%u,\"gpu_wb_pages\":%u,"
           "\"gpu_nc_pages\":%u,\"gpu_table_pages\":%u,\"numeric\":%d}\n",
           call_count,(unsigned long long)nominal.placed,(unsigned long long)nominal.jobs,
           (unsigned long long)nominal.block_jobs,(unsigned long long)nominal.block_padded,
           (unsigned long long)nominal.block_codes,(unsigned long long)nominal.block_lines,
           (unsigned long long)nominal.outputs,(unsigned long long)nominal.mismatched,
           (unsigned long long)nominal.max_jobs_per_call,(unsigned long long)nominal.zero_row_jobs,
           (double)nominal.place_lookups/call_count,(unsigned long long)(expired-1000),pins_translations,
           gpu_wb_pages,gpu_nc_pages,gpu_table_pages,numeric);
    return 0;
}
