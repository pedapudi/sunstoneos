// Actual engine readout and FreeRTOS joins; the exact-sum producer and device
// service tasks are modeled. No GPU firmware, MMIO or full recognition runs.
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "timing.h"
#include "runtime.h"
#include "parallel_runtime.h"
#include "mimus/asr/parallel.h"
#include "mimus/asr/kernels.h"
#include "mimus/asr/scratch.h"
#include "mimus/asr/exact_sum_hook.h"
#include "exact_sum_readout.h"
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

extern "C" void runtime_smp_init(void);
extern "C" void __real_MimusExactSumRows(const MimusExactSumRowsArgs *);
namespace {
using namespace mimus::asr;
constexpr size_t kRows=131,kColumns=32,kGpuColumns=16,kJobRows=64;
constexpr uint64_t kMagic=0x677075382d6a6f69;
StaticTask_t owner_state,services[3],observer_state;
StackType_t owner_stack[16384],service_stacks[3][1024],observer_stack[1024];
TaskHandle_t owner_task,service_tasks[3],observer_task;
std::atomic<unsigned> entered,readout_done,serviced,progress,failures;
std::atomic<bool> live{true},close_after_readout,returned;
alignas(64) uint8_t fast_memory[97280];
struct Producer {
    uint64_t magic=kMagic;
    const int8_t *weights;
    const int16_t *codes;
    const float *expected;
    float *output;
    size_t k,stride,weight_stride,next,previous_rows;
    unsigned width,begins,finishes;
    bool terminal,observe;
    uint64_t until;
    alignas(64) uint8_t sums[kJobRows*kGpuColumns*8];
};
Producer *borrowed;
unsigned service_core(unsigned index){return index==2 ? 6 : index;}
void service(void *argument) {
    const unsigned index=unsigned(reinterpret_cast<uintptr_t>(argument));
    configASSERT(portGET_CORE_ID()==service_core(index));
    configASSERT(uxTaskPriorityGet(nullptr)==(index==2 ? 4 : 3));
    for(;;) {
        configASSERT(ulTaskNotifyTake(pdTRUE,portMAX_DELAY)==1);
        // Count only service execution while the actual readout is held.
        configASSERT(borrowed && borrowed->observe && entered.load()==0xff);
        configASSERT(readout_done.load()==0 && !returned.load());
        serviced.fetch_or(1u<<index,std::memory_order_release);
    }
}
size_t begin(void *opaque,uint32_t ordinal,const int8_t *weights,size_t weight_stride,
             size_t rows,size_t k,size_t n,const int16_t *codes,size_t stride) {
    auto &p=*static_cast<Producer *>(opaque);
    configASSERT(xTaskGetCurrentTaskHandle()==owner_task && portGET_CORE_ID()==6);
    configASSERT(&p==borrowed && p.magic==kMagic && ordinal==7 && rows==kRows && n==kColumns);
    configASSERT(weights==p.weights && k==p.k && stride>=2*k && !(stride&3));
    p.weight_stride=weight_stride;p.codes=codes;p.stride=stride;++p.begins;
    return kGpuColumns;
}
bool finish(void *opaque,ExactSumJob *job) {
    auto &p=*static_cast<Producer *>(opaque);
    configASSERT(xTaskGetCurrentTaskHandle()==owner_task && portGET_CORE_ID()==6);
    configASSERT(p.magic==kMagic && &p==borrowed && live.load());
    if(p.previous_rows) {
        configASSERT(!p.terminal);
        // The prior sums cannot be overwritten until every actual row output
        // is final and every private adapter acknowledgement was collected.
        if(p.observe)configASSERT(readout_done.load()==0xff && serviced.load()==7);
        const size_t first=p.next-p.previous_rows;
        configASSERT(!std::memcmp(p.output+first*kColumns,p.expected+first*kColumns,
                                  p.previous_rows*kColumns*sizeof(float)));
    }
    ++p.finishes;
    if(p.next==kRows)return false;
    const size_t rows=std::min(kJobRows,kRows-p.next);
    p.observe=p.next==0;entered.store(0);readout_done.store(0);serviced.store(0);
    p.until=counter()+2*COUNTER_HZ;
    // Each job reuses the same sum region, just as the GPU producer does.
    for(size_t r=0;r<rows;++r) {
        const auto *x=reinterpret_cast<const int16_t *>(
            reinterpret_cast<const uint8_t *>(p.codes)+(p.next+r)*p.stride);
        for(size_t c=0;c<kGpuColumns;++c) {
            int64_t sum=0;
            for(size_t i=0;i<p.k;++i)sum+=int64_t{x[i]}*p.weights[c*p.weight_stride+i];
            void *out=p.sums+(r*kGpuColumns+c)*p.width;
            if(p.width==4){const int32_t s=int32_t(sum);configASSERT(s==sum);std::memcpy(out,&s,4);}
            else std::memcpy(out,&sum,8);
        }
    }
    *job={p.next,rows,p.sums,kGpuColumns*p.width,p.width};
    p.next+=rows;p.previous_rows=rows;
    return true;
}
std::vector<float> run(unsigned inner,bool residual,const std::vector<float> *expected,bool terminal=false) {
    std::vector<int8_t> weights(kColumns*inner);
    std::vector<uint16_t> scales(kColumns);
    std::vector<float> input(kRows*inner),bias(kColumns),storage(kRows*kColumns+16);
    float *output=reinterpret_cast<float *>((reinterpret_cast<uintptr_t>(storage.data())+63)&~uintptr_t(63));
    for(size_t i=0;i<weights.size();++i)weights[i]=int8_t(int(i*37%255)-127);
    for(size_t i=0;i<scales.size();++i)scales[i]=uint16_t(0x2000+i*17);
    for(size_t i=0;i<input.size();++i)input[i]=float(int(i*47%257)-128)/16;
    for(size_t i=0;i<bias.size();++i)bias[i]=float(int(i%11)-5)/8;
    for(size_t i=0;i<kRows*kColumns;++i)output[i]=-1.25f;
    TensorView tensor{};tensor.ordinal=7;tensor.dtype=DType::kI8;tensor.act_mode=ActMode::kInt16;
    tensor.rank=2;tensor.shape={kColumns,inner,0,0};tensor.k_padded=inner;tensor.row_stride=inner;
    tensor.data=reinterpret_cast<uint8_t *>(weights.data());tensor.scales=scales.data();
    Producer producer{};producer.weights=weights.data();producer.k=inner;producer.output=output;
    producer.expected=expected ? expected->data() : nullptr;producer.width=inner>512 ? 8 : 4;producer.terminal=terminal;
    const ExactSumHook hook{begin,finish,&producer};
    if(expected){borrowed=&producer;SetExactSumHook(&hook);}
    SetInternalArena(fast_memory);
    Arena arena({reinterpret_cast<uint8_t *>(0x46000000),12*1024*1024});
    const auto result=residual ? LinearAccumulate(tensor,bias.data(),input.data(),kRows,inner,output,kColumns,0.5f,arena)
                              : Linear(tensor,bias.data(),input.data(),kRows,inner,output,kColumns,arena);
    configASSERT(!terminal && result.has_value());
    if(expected) {
        configASSERT(producer.begins==1 && producer.finishes==4);
        configASSERT(!std::memcmp(output,expected->data(),kRows*kColumns*sizeof(float)));
        SetExactSumHook(nullptr);borrowed=nullptr;
    }
    SetInternalArena({});
    return {output,output+kRows*kColumns};
}
void observe(void *) {
    configASSERT(ulTaskNotifyTake(pdTRUE,portMAX_DELAY)>=1);
    vTaskDelay(3);
    configASSERT(failures.load() && !live.load() && !returned.load());
    configASSERT(borrowed && borrowed->magic==kMagic && borrowed->terminal);
    configASSERT(borrowed->finishes==1 && borrowed->next==kJobRows);
    configASSERT(entered.load()==0xff && serviced.load()==7);
    platform_puts("PASS GPU8 workers: exact readout joins; A55 and GPU service progress; pre-ack failure retains sums and owner\n");
    platform_exit(0);
}
void recognize(void *) {
    configASSERT(portGET_CORE_ID()==6 && uxTaskPriorityGet(nullptr)==kMimusOwnerPriority);
    const auto narrow=run(176,false,nullptr),wide=run(528,true,nullptr);
    mimus_parallel_bind();
    configASSERT(!AsyncAvailable());
    run(176,false,&narrow);run(528,true,&wide);
    mimus_parallel_report();
    platform_puts("PASS GPU8 arithmetic: int32 partial channels and int64 residual; three jobs including short tail\n");
    run(176,false,&narrow,true);
    returned.store(true);platform_panic("owner returned without readout acknowledgement",0);
}
}
extern "C" void __wrap_MimusExactSumRows(const MimusExactSumRowsArgs *args) {
    configASSERT(borrowed && borrowed->magic==kMagic);
    const unsigned core=portGET_CORE_ID();
    configASSERT(uxTaskPriorityGet(nullptr)==(core<6 ? 2 : 3));
    if(borrowed->observe) {
        configASSERT(args->rows==8);
        const unsigned prior=entered.fetch_or(1u<<core,std::memory_order_release);
        configASSERT(!(prior&(1u<<core)));
        if(core==6) {
            while(entered.load(std::memory_order_acquire)!=0xff)configASSERT(counter()<borrowed->until);
            for(auto service:service_tasks)xTaskNotifyGive(service);
        }
        while(serviced.load(std::memory_order_acquire)!=7)configASSERT(counter()<borrowed->until);
    }
    __real_MimusExactSumRows(args);
    if(borrowed->observe) {
        readout_done.fetch_or(1u<<core,std::memory_order_release);
        if(borrowed->terminal && core==0)close_after_readout.store(true,std::memory_order_release);
    }
}
extern "C" int mimus_board_live(void) {
    if(portGET_CORE_ID()==0 && close_after_readout.load(std::memory_order_acquire))live.store(false);
    return live.load(std::memory_order_acquire);
}
extern "C" void mimus_board_fail(void) {
    live.store(false,std::memory_order_release);failures.fetch_add(1);xTaskNotifyGive(observer_task);
}
extern "C" void mimus_board_progress(void){progress.fetch_add(1);}
extern "C" int os_thermal_throttled(void){return 0;}
extern "C" void vApplicationStackOverflowHook(TaskHandle_t,char *name){platform_panic(name,0);}
extern "C" void kernel_main(void) {
    platform_init();runtime_smp_init();runtime_init();mimus_parallel_init(6,7);
    for(unsigned i=0;i<3;++i) {
        service_tasks[i]=xTaskCreateStaticAffinitySet(service,"service",1024,reinterpret_cast<void *>(uintptr_t(i)),
            i==2 ? 4 : 3,service_stacks[i],&services[i],1u<<service_core(i));
        configASSERT(service_tasks[i]);
    }
    observer_task=xTaskCreateStaticAffinitySet(observe,"retention",1024,nullptr,5,observer_stack,&observer_state,1);
    configASSERT(observer_task);
    owner_task=xTaskCreateStaticAffinitySet(recognize,"recognizer",16384,nullptr,kMimusOwnerPriority,owner_stack,&owner_state,1u<<6);
    configASSERT(owner_task);vTaskStartScheduler();platform_panic("scheduler returned",0);
}
