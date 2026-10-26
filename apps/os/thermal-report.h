#ifndef SUNSTONE_OS_THERMAL_REPORT_H
#define SUNSTONE_OS_THERMAL_REPORT_H
#include <stddef.h>
#include <stdint.h>
/* CPU-clock qualification only. Operation 7 uses the existing 64-byte USB
 * response header and at most 1024 payload bytes per request. */
#define OS_THERMAL_REPORT_OP 7u
#define OS_THERMAL_REPORT_MAGIC UINT32_C(0x5448524d)
struct os_thermal_reading {
    uint64_t time_us;
    int32_t millicelsius[5];
    uint32_t age_us[5],sequence;
    uint8_t seen,stale,level,sensor;
};
struct os_thermal_report {
    uint32_t magic,version,bytes,rows;
    uint64_t epoch,generation,recognition_generation,begin_us,end_us;
    uint32_t overflow,cpu_critical_millicelsius,row_bytes,recognition_ok;
};
_Static_assert(sizeof(struct os_thermal_reading)==56,"thermal row geometry");
_Static_assert(sizeof(struct os_thermal_report)==72,"thermal report geometry");
/* First recognition only. Its header/range and rows remain immutable until
 * power removal. A zero expected generation selects that first window; later
 * requests bind its recognition generation. Pending returns 0; invalid -1. */
int os_thermal_report_copy(uint64_t expected,uint32_t offset,size_t bytes,
    uint8_t *out,size_t *copied,uint32_t *total);
#endif
