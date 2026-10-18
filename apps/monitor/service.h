#ifndef SUNSTONE_MONITOR_SERVICE_H
#define SUNSTONE_MONITOR_SERVICE_H
#include <stdint.h>
enum monitor_status {MONITOR_STARTING,MONITOR_AGAIN,MONITOR_SNAPSHOT,
 MONITOR_STOPPING,MONITOR_STOPPED,MONITOR_FAILED};
/* Latest complete per-device sets, observed asynchronously. Raw ADC codes do
 * not identify board straps or calibrated voltages. Event masks are sticky
 * observations, not event counts. Every timestamp is monotonic microseconds. */
struct monitor_snapshot {
 uint64_t generation,observed_us,thermal_sets,adc_sets;
 uint64_t thermal_times[5],adc_times[4];
 int32_t thermal_millicelsius[5];
 uint32_t thermal_sequence,adc_sequence,cldo3_uv,parent_word,state;
 uint16_t thermal_codes[5],adc_codes[4];
 uint16_t factory_tenth_c,factory_codes[5],corrections[5];
 uint8_t alarm,alarm_off,critical,reserved[5];
};
_Static_assert(sizeof(struct monitor_snapshot)==192,"monitor snapshot geometry");
/* Task context only. Copies a coherent complete snapshot only when generation exceeds after.
 * Output is unchanged otherwise. Caller owns storage through return. */
enum monitor_status monitor_read_snapshot(struct monitor_snapshot *,uint64_t after);
/* Task context only. Idempotent bounded request; owners subsequently verify configured disable.
 * STOPPED requires both acknowledgments, keeps all resources permanently and
 * permits no restart. A stalled MMIO cannot be recovered by this service. */
enum monitor_status monitor_request_stop(void);
#endif
