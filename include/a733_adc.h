#ifndef SUNSTONE_A733_ADC_H
#define SUNSTONE_A733_ADC_H
#include "a733_clock_leases.h"
#define A733_ADC_CHANNELS 4u
#define A733_ADC_INTID 70u /* A733 DTS SPI38, not the generic manual IRQ93. */
enum a733_adc_result {A733_ADC_PENDING,A733_ADC_BUSY,A733_ADC_READY,A733_ADC_AGAIN,A733_ADC_SAMPLE,A733_ADC_RETAINED,A733_ADC_INVALID,A733_ADC_STOPPED};
struct a733_adc_config {uint64_t owner,handoff,startup_deadline_us;uint32_t apb1,apb1_hz;unsigned configure_adc_clock;};
/* mask selects channels0..3 for compare windows (manual18.12.8.14): low
 * pending reports a code under low[i], high pending a code over high[i].
 * Each selected window needs low<high<=4095; unselected channels keep zero. */
struct a733_adc_thresholds {unsigned mask;uint16_t low[A733_ADC_CHANNELS],high[A733_ADC_CHANNELS];};
struct a733_adc_sample {uint64_t observed_us[A733_ADC_CHANNELS];uint32_t sequence,valid;uint16_t code[A733_ADC_CHANNELS];};
struct a733_adc_ops {
 int (*now_us)(void *,uint64_t *);
 int (*current)(void *);
 /* Authenticate exact token/config, passive board straps0..3, input0..VCC-ADC,
  * safe/stable AVCC and internal reference, source impedance<=1Mohm, no test
  * injection/FIFO/DMA/old handlers/remote writers, mappings/security and qualified
  * recovery. INTID70 is owned/masked before startup. Cold parent setup excludes
  * every THS/ADC consumer. Shared GPADC24M permits adoption only, never retune.
  * Supervised permanent retention/manual power removal is permitted; software
  * deadlines cannot recover stalled MMIO. No pad or reference writes are allowed. */
 int (*authorize)(void *,const struct a733_adc_config *,const struct a733_clock_token *);
 /* enter:1 acquired,0 busy,negative denial without ownership. */
 int (*enter)(void *,uint64_t);void (*leave)(void *);void (*retain)(void *);
 int (*read)(void *,uintptr_t,uint32_t *);int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_adc {
 struct a733_adc_config config;struct a733_adc_ops ops;struct a733_clock_token token;
 struct a733_clock_leases *registry;void *cookie;atomic_uint busy;struct a733_adc_thresholds thresholds;
 uint64_t deadline,last_now;uint32_t sequence,low_seen,high_seen;unsigned initialized,stage,fault,held;
};
/* Private zero-once objects and token are disjoint and permanent. GPADC is
 * exclusive; REF/OSC/FABRIC and GPADC24M stable, PERI0 stable for APB1 parent3.
 * configure_adc_clock additionally requires exclusive GPADC24M. Callbacks are
 * bounded/nonreentrant. No reset assertion, release or restart is provided. */
int a733_adc_init(struct a733_adc *,const struct a733_adc_config *,const struct a733_adc_ops *,void *,struct a733_clock_leases *,const struct a733_clock_token *);
/* Owner task, after init and before the first poll; no MMIO. Selects compare
 * windows that startup programs before enabling the converter. Without this
 * call the passive profile requires every compare enable and status clear. */
int a733_adc_configure_thresholds(struct a733_adc *,const struct a733_adc_thresholds *);
enum a733_adc_result a733_adc_poll(struct a733_adc *);
/* Task-only deferred service. Copy each observed selected channel before W1C
 * only its observed flag. Values are latest observations, not synchronized or
 * lossless conversions; flags may reassert while reading/W1C. Zero is valid.
 * Caller handles GIC mask/rearm/EOI. Output remains unchanged without SAMPLE.
 * Calls before READY or with a zero deadline return INVALID without I/O; an
 * expired deadline or non-owner caller still faults and retains.
 * Terminal fault retries retention metadata only, never healthy I/O. */
enum a733_adc_result a733_adc_read(struct a733_adc *,uint64_t,struct a733_adc_sample *);
/* Owner-task-only, no MMIO. read() also copies and W1C-clears only observed
 * selected low/high compare statuses; they may reassert while the input stays
 * outside its window. This returns and clears the channel bits latched since
 * the previous call: SAMPLE when any is set, AGAIN when none. Without
 * configured windows it returns INVALID. A non-owner caller faults and retains. */
enum a733_adc_result a733_adc_thresholds(struct a733_adc *,uint32_t *low,uint32_t *high);
/* Owner-task-only configured disable after READY. Caller masks its GIC IRQ
 * before entry and supplies a fresh absolute deadline. STOPPED means converter
 * and local interrupt enables were read back disabled; it does not prove an
 * in-flight analog conversion drained. All clocks, resets, memory and leases
 * remain owned. No restart/release is provided. Uncertainty faults permanently;
 * BUSY requires checking the owner-only fault latch before retry. Repeated
 * STOPPED calls and later poll/read calls perform no callbacks or MMIO. */
enum a733_adc_result a733_adc_stop(struct a733_adc *,uint64_t deadline);
#endif
