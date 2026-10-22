/* Placement of mimus linear layers on the CPU or the GPU
 * (docs/recognition-dispatch.md). Every function except the mapping check
 * runs in the recognizer task at EL0, which also runs the GPU hook, so the
 * active count and the record need no atomics or locks. */
#include "dispatch.h"
#include "a733_mapping_check.h"
#include "placement.h"

struct os_dispatch_record os_dispatch_record;
/* checks/os-image-link.py compares the INTID 95 route's core with this. */
__attribute__((used,retain)) const uint8_t os_recognition_core=OS_RECOGNITION_CORE;
/* Rows below this count are active: the table's length while the session is
 * open, zero before OPEN, after a refused OPEN and after a GPU fault. It has
 * external linkage so that the lookup stays one load and one compare instead
 * of a flag test plus a compare with the table length. */
uint32_t os_dispatch_active;

int os_dispatch_open(void)
{
    if(os_dispatch_record.opened||os_dispatch_record.open_failed)return os_dispatch_record.opened;
    if(a733_gpu_mimus_open()!=1){os_dispatch_record.open_failed=1;return 0;}
    os_dispatch_record.opened=1;os_dispatch_active=OS_PLACEMENT_TENSORS;return 1;
}
struct os_placement os_dispatch_place(uint32_t ordinal)
{
    return ordinal<os_dispatch_active?os_placement_table[ordinal]:(struct os_placement){0,0};
}
void os_dispatch_placed(void){++os_dispatch_record.placed;}
void os_dispatch_refused(void){++os_dispatch_record.refused;}
void os_dispatch_incomplete(void){os_dispatch_record.incomplete=1;os_dispatch_active=0;}
void os_dispatch_failed(unsigned state)
{
    /* The models and the code span are trustworthy after a GPU fault only if
     * the GPU MMU enforces its read-only mappings, which the board has not
     * shown; RESET therefore ends the boot like LOST. */
    os_dispatch_record.failure=state==OS_DISPATCH_RESET?OS_DISPATCH_RESET:OS_DISPATCH_LOST;
    os_dispatch_active=0;
    os_dispatch_stop();
}

int os_dispatch_el0_read_only(os_dispatch_translate_fn translate,uintptr_t address,size_t bytes)
{
    static const struct a733_mapping_rule readable={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!translate||!bytes||(address&4095)||(bytes&4095)||bytes>UINTPTR_MAX-address)return 0;
    for(size_t offset=0;offset<bytes;offset+=4096){
        struct aarch64_translation t={0};
        if(!a733_mapping_check(translate,address+offset,address+offset,&readable)||translate(address+offset,1,&t))
            return 0;
    }
    return 1;
}

/* The gpu-linear start entry (apps/os/start.c): the GPU runtime's EL1 worker
 * on the recognition core, created before the scheduler starts. */
int os_gpu_linear_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==a733_gpu_mimus_worker;}
int os_gpu_linear_pins(struct a733_pin_handoff *pins){return pins!=NULL;}
int os_gpu_linear_start(void){return a733_gpu_mimus_start(OS_RECOGNITION_CORE)==1;}
