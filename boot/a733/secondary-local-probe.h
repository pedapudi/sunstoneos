#ifndef A733_SECONDARY_LOCAL_PROBE_H
#define A733_SECONDARY_LOCAL_PROBE_H
#include <stdint.h>
#define SECONDARY_LOCAL_RECORDS 0x42018a00
#define SECONDARY_DISTRIBUTOR_RECORD 0x42019a00
struct secondary_local_state {
    uint64_t ctlr,waker,enable,pending,active,group,modifier,configuration,priority,pwrr;
};
struct secondary_local_access { uint64_t address,value,operation,completed; };
struct secondary_local_record {
    uint64_t state,epoch,core,deadline,base,control_before,control_after;
    uint64_t controller_state,phase,error,writes,cleanup_error,cleanup_writes;
    uint64_t sre_before,sre_after,group0,group1;
    struct secondary_local_state before,after;
    struct secondary_local_access access,controller_access;
    uint64_t reserved[19];
};
_Static_assert(sizeof(struct secondary_local_record)==512,"Per-core local probe report ABI");
/* Returning diagnostic only. Distributor configuration and exclusive CPU,
 * redistributor, report and interrupt ownership precede this call. The target
 * is at EL3 with masked exceptions, no caches and its actual 24MHz counter setup.
 * The target's frame must already be powered, as in the observed core-1 path.
 * Return1 means initialization and restoration passed; return2 means a recorded
 * bounded refusal with unchanged/restored local state; return0 retains ownership.
 * Neither success nor refusal establishes interrupt delivery. */
struct secondary_distributor_record {
    uint64_t state, before, after, banks, deadline;
    uint32_t enable_before[32], enable_after[32];
    uint64_t reserved[27];
};
_Static_assert(sizeof(struct secondary_distributor_record)==512,"Distributor report ABI");
int secondary_distributor_begin(uint64_t deadline);
int secondary_distributor_restore(uint64_t deadline);
int secondary_local_probe(unsigned core,uint64_t epoch,uint64_t deadline,
                          volatile struct secondary_local_record *record);
#endif
