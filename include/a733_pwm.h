#ifndef SUNSTONE_A733_PWM_H
#define SUNSTONE_A733_PWM_H
#include "a733_clock_leases.h"
#include "a733_pins.h"
#include "a733_pmic.h"
#define A733_PWM_BASE UINT32_C(0x02527000)
#define A733_PWM_INTID 67u
/* Fixed board consumer: whole PWM0 is owned, output is channel5/PD5 mux7.
 * Only SYS_CLK24M, finite pulses and an unreleased output-low stop are used. */
struct a733_pwm_config {
 uint64_t owner,handoff,deadline_us;
 uint32_t apb0,apb0_hz,period_ns,duty_ns,pulses,active_high;
 struct a733_pin_config expected,desired;
};
struct a733_pwm_plan { uint32_t pcr,ppr,ticks,active;uint64_t duration_us; };
/* Pure exact-tick planning; no rounding, hardware or pointer authority. */
int a733_pwm_plan(const struct a733_pwm_config *,struct a733_pwm_plan *,a733_clock_mask_t *);
struct a733_pwm_ops {
 int (*now_us)(void *,uint64_t *);
 int (*current)(void *);
 int (*admit)(void *,const struct a733_pwm_config *,const struct a733_clock_token *);
 int (*enter)(void *,uint64_t);
 void (*leave)(void *);
 void (*retain)(void *);
 int (*read)(void *,uintptr_t,uint32_t *);
 int (*write)(void *,uintptr_t,uint32_t);
 int (*outside)(void *,const void *,size_t);
};
enum a733_pwm_result { A733_PWM_PENDING,A733_PWM_BUSY,A733_PWM_ARMED,
 A733_PWM_RUNNING,A733_PWM_COMPLETE,A733_PWM_RETAINED,A733_PWM_INVALID };
struct a733_pwm_report {
 uint32_t phase,error,started,irq_seen,pisr,pcr,ppr,counter,pulse_counter;
 uint32_t waveform_stopped,gpio_low,bgr,apb0,voltage_mode,voltage_value;
 uint32_t reserved;
 uint64_t started_us,completed_us;
};
struct a733_pwm {
 struct a733_pwm_config config;
 struct a733_pwm_plan plan;
 struct a733_pwm_ops ops;
 void *cookie;
 struct a733_clock_leases *clocks;
 struct a733_pins *pins;
 struct a733_rail_leases *rails;
 struct a733_pmic *pmic;
 struct a733_clock_token clock;
 struct a733_pin_lease_banked pin;
 struct a733_rail_token rail;
 struct a733_pmic_lifetime power;
 struct a733_pmic_request request;
 struct a733_pmic_snapshot supply;
 struct a733_pwm_report report;
 a733_clock_mask_t shared;
 atomic_uint busy;
 uint64_t last_now;
 uint32_t initial_bgr;
 unsigned initialized,fault,held,clock_retained,rail_retained;
};
/* Zero once. Actual managers/adapters/config storage must be disjoint and live
 * permanently. Init is pure. Physical admission establishes cold controller,
 * stable 1.8V BLDO1/PD bank, 5V fan topology/loading, no remote/source/pad/voltage
 * writer and operation-specific recovery. No close/retry/reset API exists. */
int a733_pwm_init(struct a733_pwm *,const struct a733_pwm_config *,const struct a733_pwm_ops *,void *,
 struct a733_clock_leases *,struct a733_pins *,struct a733_rail_leases *,struct a733_pmic *);
enum a733_pwm_result a733_pwm_poll(struct a733_pwm *,unsigned irq_notice);
/* Caller has configured/rearmed the actual primary Group1NS level SPI67.
 * Hardware pulse start is last, after complete plan and control readbacks. */
enum a733_pwm_result a733_pwm_start(struct a733_pwm *);
/* Owner-only metadata after revocation, including expiry. BUSY bookkeeping
 * retries are explicit. A retained result never claims the fan stopped. */
enum a733_pwm_result a733_pwm_retain(struct a733_pwm *);
#endif
