#ifndef SUNSTONE_A733_THERMAL_H
#define SUNSTONE_A733_THERMAL_H
#include <stdatomic.h>
#include <stdint.h>
#include "a733_clock_leases.h"
#define A733_THERMAL_SENSORS 5u
#define A733_THERMAL_INTID 71u /* GIC INTID, equivalent to SPI 39. */
enum a733_thermal_result { A733_THERMAL_PENDING, A733_THERMAL_BUSY,
    A733_THERMAL_READY, A733_THERMAL_AGAIN, A733_THERMAL_SAMPLE,
    A733_THERMAL_RETAINED, A733_THERMAL_INVALID, A733_THERMAL_STOPPED };
/* Physical channel order: CPUB, DDR, NPU, CPUL, GPU. Thresholds use calibrated
 * 12-bit hardware codes. Lower codes indicate hotter temperatures within each branch. The source
 * temperature formula has a discontinuity at code 1769; no smooth inverse is
 * implied by this interface. The board policy below compares converted readings. */
struct a733_thermal_limits { uint16_t hot, hysteresis, critical; };
struct a733_thermal_config {
    uint64_t owner, handoff, startup_deadline_us;
    uint32_t apb1, apb1_hz; /* Masked CCU0518; SYS24M or PERI0_600 only. */
    uint8_t configure_adc_clock, alarm_mask, critical_mask;
    struct a733_thermal_limits limits[A733_THERMAL_SENSORS];
};
struct a733_thermal_calibration {
    uint16_t factory_tenth_c, factory_code[A733_THERMAL_SENSORS];
    uint16_t correction[A733_THERMAL_SENSORS];
};
struct a733_thermal_sample {
    uint64_t observed_us;
    uint32_t sequence;
    uint8_t valid, alarm, alarm_off, critical;
    uint16_t code[A733_THERMAL_SENSORS];
    int32_t millicelsius[A733_THERMAL_SENSORS];
};
struct a733_thermal_ops {
    int (*now_us)(void *,uint64_t *);
    /* Authenticate exact live token/owner/handoff; APB1 stability, AVCC/ADC and
     * digital supplies; exclusive THS and remote-writer handoff; readable SID
     * shadows; Device mappings and register security permissions;
     * operation-specific recovery (qualified independent reset, or reviewed
     * supervised permanent retention until confirmed whole-board power removal);
     * GIC INTID71 owned/masked. A software deadline cannot recover a stalled
     * MMIO access; the selected physical recovery path must be qualified.
     * Cold GPADC configuration additionally excludes ALL GPADC/THS consumers.
     * IRQ delivery is allowed only after the caller binds its actual GIC route
     * and deferred service. This callback supplies authority, not clock evidence. */
    int (*authorize)(void *,const struct a733_thermal_config *,const struct a733_clock_token *);
    int (*enter)(void *,uint64_t); /* 1 held; 0 busy; other result uncertain. */
    void (*leave)(void *);
    /* Entry uncertainty may be unowned. Check actual task/mutex ownership
     * before forwarding retention; preserve known-held faults even when the
     * health predicate has failed. Never release or resume register access. */
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_thermal {
    struct a733_thermal_config config;
    struct a733_thermal_ops ops;
    struct a733_clock_token token;
    struct a733_clock_leases *registry;
    struct a733_thermal_calibration calibration;
    void *cookie;
    atomic_uint busy;
    uint64_t deadline,last_now;
    uint32_t sequence;
    unsigned initialized,stage,fault,held;
};
/* Pure A733-specific packed eFuse decoding. Missing/erased sensor words or an
 * unrepresentable correction fail without changing output; no guessed trim.
 * The final word's upper 24 bits belong to other functions and are ignored. */
int a733_thermal_calibrate(const uint32_t words[3],struct a733_thermal_calibration *);
int a733_thermal_temperature(uint16_t code,int32_t *millicelsius);
/* Zero private storage once. Object/config/ops/token/registry must be disjoint
 * and remain alive. Borrow the actual registry token immutably for this entire
 * lifetime: THS exclusive, GPADC24M stable or exclusive, FABRIC/REF/OSC stable,
 * and PERI0 stable when APB1 selects it. Configuring GPADC requires exclusive
 * GPADC24M. All overlapping writers must use these scopes and one register mutex.
 * Callbacks are bounded, non-reentrant and return exactly 1 on success;
 * authorize/enter may return 0 pending/busy. Time is monotonic microseconds.
 */
int a733_thermal_init(struct a733_thermal *,const struct a733_thermal_config *,
    const struct a733_thermal_ops *,void *,struct a733_clock_leases *,const struct a733_clock_token *);
enum a733_thermal_result a733_thermal_poll(struct a733_thermal *);
/* Task context only; an ISR can schedule this bounded deferred service.
 * Read conversion/event flags, collect valid samples and W1C only observed bits.
 * This is latest-value reporting, not an event counter or synchronized sampling.
 * Status can reassert immediately; successful W1C does not prove IRQ deassertion.
 * Output is unchanged on AGAIN/BUSY/error. Caller services GIC mask/rearm/EOI.
 * A SAMPLE can contain only events with valid=0. Never use an invalid channel.
 */
enum a733_thermal_result a733_thermal_read(struct a733_thermal *,uint64_t deadline,
    struct a733_thermal_sample *);
/* No release or voltage change API. Faults keep
 * all authority, with metadata-only retry if registry retention returns BUSY.
 * Terminal fault polls/read calls perform no callbacks or hardware access. */
/* Owner-task-only configured disable after READY. Caller masks its GIC IRQ
 * before entry and supplies a fresh absolute deadline. STOPPED means converter
 * and local interrupt enables were read back disabled; it does not prove an
 * in-flight analog conversion drained. All clocks, resets, memory and leases
 * remain owned. No restart/release is provided. Uncertainty faults permanently;
 * BUSY requires checking the owner-only fault latch before retry. Repeated
 * STOPPED calls and later poll/read calls perform no callbacks or MMIO. */
enum a733_thermal_result a733_thermal_stop(struct a733_thermal *,uint64_t deadline);
/* Fixed board thermal policy over a733_thermal_read results. Trips are the
 * pinned sun60iw2p1 thermal zones: CPUB, CPUL and GPU throttle at 90 C (CPU
 * hysteresis 2 C, GPU none) and every channel is critical at 110 C. DDR also
 * throttles at 85 C with 2 C hysteresis, a board trip for the LPDDR5
 * standard-refresh limit; the THS DDR channel is a proxy with an unknown
 * offset from the DRAM die. All trips lie in the code<=1769 branch, so the
 * conversion discontinuity cannot reorder them. Hardware alarm/critical
 * events are not inputs: their raw thresholds are per-session configuration. A channel with no valid reading for 1 s
 * throttles and for 5 s is critical. Controller failure, an unexpected result,
 * a replayed sample or backward time is critical. CRITICAL is terminal until
 * power removal. The policy changes no clock, voltage, power or THS state. */
enum a733_thermal_level { A733_THERMAL_NORMAL, A733_THERMAL_THROTTLE, A733_THERMAL_CRITICAL };
struct a733_thermal_policy {
    /* Owner task, once per level change. THROTTLE asks for reduced load that
     * still makes watchdog-credited progress; NORMAL lifts it. Both are hints
     * and the result is ignored. For CRITICAL return 1 once new work is refused
     * and a stop or power-off request is issued, 0 to be asked again on the
     * next update, or any other value for an uncertain outcome, which is
     * retained and never asked again (request 1 pending, 2 done, 3 uncertain). */
    int (*notify)(void *,enum a733_thermal_level);
    void *cookie;
    atomic_uint level;
    uint64_t start_us,last_us,seen_us[A733_THERMAL_SENSORS];
    int32_t millicelsius[A733_THERMAL_SENSORS];
    uint32_t sequence,cpu_critical_millicelsius;
    /* Channel masks: valid reading seen, at passive trip, without a reading
     * for 1 s, critical (trip reached or no reading for 5 s). Frozen at CRITICAL. */
    uint8_t seen,passive,stale,critical,sensor;
    unsigned initialized,request;
    atomic_uint published_seen; /* seen, published after the level for other tasks. */
};
/* Zeroed storage once; now_us starts every channel's staleness window. */
int a733_thermal_policy_init(struct a733_thermal_policy *,uint64_t now_us,
    int (*notify)(void *,enum a733_thermal_level),void *cookie);
/* Before the first update, optionally lower the CPU channels (CPUB 0, CPUL 3)
 * critical trip. Zero selects the vendor trip; values above it are refused. */
int a733_thermal_policy_cpu_limit(struct a733_thermal_policy *,uint32_t millicelsius);
/* Owner task only, after each a733_thermal_read, with now_us from the driver's
 * clock read after that call. The sample is read only for SAMPLE. Bounded: at
 * most one notify call. Returns the level now in force. */
enum a733_thermal_level a733_thermal_policy_update(struct a733_thermal_policy *,uint64_t now_us,
    enum a733_thermal_result,const struct a733_thermal_sample *);
/* Any task or core, for example a scheduler choosing its worker count. */
enum a733_thermal_level a733_thermal_policy_level(const struct a733_thermal_policy *);
/* Any task or core: the channels that have had at least one valid reading,
 * as bits in physical channel order. A level read after this call reflects
 * at least the update that published these channels. Zero before
 * initialization. */
unsigned a733_thermal_policy_seen(const struct a733_thermal_policy *);
#endif
