/* Placement of mimus linear layers on the CPU or the GPU
 * (docs/recognition-dispatch.md). Every function except the mapping check
 * runs in the recognizer task at EL0, which also runs the GPU hook, so the
 * active count and the record need no atomics or locks. */
#include "dispatch.h"
#include "a733_mapping_check.h"
#include "placement.h"
#include <stdatomic.h>
int os_thermal_throttled(void);

struct os_dispatch_record os_dispatch_record;
/* checks/os-image-link.py compares the INTID 95 route's core with this. */
__attribute__((used,retain)) const uint8_t os_recognition_core=OS_RECOGNITION_CORE;
/* Table lengths for the GPU runtime (apps/os/gpu.c), which admits the
 * tensors at start without including the generated table a second time. */
const uint32_t os_placement_rows=OS_PLACEMENT_TENSORS,os_placement_gpu_rows=OS_PLACEMENT_GPU_TENSORS;
/* Rows below this count are active: the table's length while the session is
 * open, zero before OPEN, after a refused OPEN and after a GPU fault. It has
 * external linkage so that the lookup stays one load and one compare instead
 * of a flag test plus a compare with the table length. */
uint32_t os_dispatch_active;
/* OS_DISPATCH_GPU_WAITING during OPEN and each job's wait,
 * OS_DISPATCH_GPU_FAULT from a GPU fault on. The supervisor reads it
 * (apps/os/services.c). */
static atomic_uint gpu_state;
unsigned os_dispatch_gpu_state(void){return atomic_load_explicit(&gpu_state,memory_order_acquire);}

int os_dispatch_open(void)
{
    if(os_dispatch_record.opened||os_dispatch_record.open_failed)return os_dispatch_record.opened;
    atomic_store_explicit(&gpu_state,OS_DISPATCH_GPU_WAITING,memory_order_release);
    if(a733_gpu_mimus_open()!=1){
        os_dispatch_record.open_failed=1;atomic_store_explicit(&gpu_state,OS_DISPATCH_GPU_IDLE,memory_order_release);return 0;
    }
    atomic_store_explicit(&gpu_state,OS_DISPATCH_GPU_IDLE,memory_order_release);
    os_dispatch_record.opened=1;os_dispatch_active=OS_PLACEMENT_TENSORS;return 1;
}
struct os_placement os_dispatch_place(uint32_t ordinal)
{
    return ordinal<os_dispatch_active&&!os_thermal_throttled()?os_placement_table[ordinal]:(struct os_placement){0,0};
}
void os_dispatch_placed(void){++os_dispatch_record.placed;}
void os_dispatch_refused(void){++os_dispatch_record.refused;}
void os_dispatch_incomplete(void)
{os_dispatch_record.incomplete=1;os_dispatch_active=0;}
void os_dispatch_waiting(int waiting)
{atomic_store_explicit(&gpu_state,waiting?OS_DISPATCH_GPU_WAITING:OS_DISPATCH_GPU_IDLE,memory_order_release);}
void os_dispatch_failed(unsigned state)
{
    /* The models and the code span are trustworthy after a GPU fault only if
     * the GPU MMU enforces its read-only mappings, which the board has not
     * shown; RESET therefore ends the boot like LOST. */
    os_dispatch_record.failure=state==OS_DISPATCH_RESET?OS_DISPATCH_RESET:OS_DISPATCH_LOST;
    os_dispatch_active=0;
    atomic_store_explicit(&gpu_state,OS_DISPATCH_GPU_FAULT,memory_order_release);
    os_dispatch_stop();
}

/* Every 4 KiB page with a 4 KiB stride. With a block stride, the first and
 * the last 4 KiB page of each block: one block descriptor maps every page of
 * its block alike, and the two pages show that no entry of the range became
 * a table with pages of its own at either end. */
int os_dispatch_read_only(os_dispatch_translate_fn el0,os_dispatch_translate_fn el1,uintptr_t address,size_t bytes,
                          uintptr_t stride)
{
    static const struct a733_mapping_rule readable={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!el0||!el1||!bytes||stride<4096||(stride&(stride-1))||((address|bytes)&(stride-1))||bytes>UINTPTR_MAX-address)
        return 0;
    for(uintptr_t block=address;block-address<bytes;block+=stride)
        for(uintptr_t page=block;;page=block+stride-4096){
            struct aarch64_translation t={0};
            if(!a733_mapping_check(el0,page,page,&readable)||el0(page,1,&t)||el1(page,1,&t))return 0;
            if(page==block+stride-4096)break;
        }
    return 1;
}
