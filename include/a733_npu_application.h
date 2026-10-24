#ifndef SUNSTONE_A733_NPU_APPLICATION_H
#define SUNSTONE_A733_NPU_APPLICATION_H
#include "a733_npu_reserved.h"
#include "a733_pmic_native.h"
#include "a733_pins_native.h"

#define A733_NPU_APPLICATION_COMMAND UINT32_C(0x33)
#define A733_NPU_APPLICATION_FRAME UINT64_C(0x415337334e505531)

/* Immutable identities of the authenticated session record. The evidence
 * identities name reviewed records; the owners are the clock, rail and
 * provider owners of the image. None establishes a physical fact itself. */
struct a733_npu_application_profile {
    uint64_t npu_evidence,command_admission,usb_evidence,pmic_evidence,pin_evidence;
    uint64_t npu_owner,pmic_owner,usb_owner,bridge_owner;
};
#ifdef SUNSTONE_NPU_FINITE_THERMAL
/* Consumed polling-controller configuration and four authenticated records. */
struct a733_npu_thermal_boot {
    uint64_t owner,authority;
    uint8_t initial[32],electrical[32],clocks[32],supervision[32];
    uint32_t apb1,apb1_hz;
    uint8_t configure_adc_clock,reserved[7];
};
_Static_assert(sizeof(struct a733_npu_thermal_boot)==160,"finite thermal wire record");
#endif
struct a733_npu_application_boot {
    struct a733_npu_application_profile profile;
    /* The fabric record; the PMIC startup deadline must be zero in this
     * input, and initialization derives it from the boot contract. */
    struct a733_npu_platform_config npu;
    struct a733_pmic_config pmic;
    struct a733_pin_handoff pins;
#ifdef SUNSTONE_NPU_FINITE_THERMAL
    struct a733_npu_thermal_boot thermal;
#endif
};
const struct a733_npu_application_boot *a733_npu_application_boot_record(void);
extern const unsigned char a733_npu_application_model[];
extern const unsigned char a733_npu_application_input[];
extern const unsigned char a733_npu_application_reference[];
extern const size_t a733_npu_application_model_bytes, a733_npu_application_input_bytes, a733_npu_application_reference_bytes;
extern const unsigned char a733_npu_application_artifact_hashes[3][32];

/* Client alone writes output/timing, then release-publishes complete. Status1
 * means both copied results matched the selected reference and were reaped;
 * status2 means at least one completed result differed. Zero remains pending. */
struct a733_npu_application_result {
    atomic_uint complete;
    uint64_t start,end,job,previous_job;
    unsigned jobs;
#ifdef SUNSTONE_NPU_FINITE_THERMAL
    unsigned thermal_reason;
#endif
    unsigned char first_digest[32];
    unsigned char output[NPU_CONVOLUTION_IO_CAPACITY];
};
extern struct a733_npu_application_result a733_npu_application_result;
extern atomic_uint a733_npu_application_failed;

int a733_npu_application_init(TaskHandle_t client);
struct npu_convolution *a733_npu_application_client(void);
unsigned a733_npu_application_ready(void);
#ifdef SUNSTONE_NPU_FINITE_THERMAL
unsigned a733_npu_application_thermal_live(void);
unsigned a733_npu_application_thermal_claim(void);
void a733_npu_application_thermal_complete(void);
void a733_npu_application_thermal_close(void);
unsigned a733_npu_application_thermal_reason(void);
uint64_t a733_npu_application_thermal_deadline(void);
unsigned a733_npu_application_never_started(void);
#endif
/* Finite adapter only: original bootstrap publishes after matched CLOSE and
 * successful local gate/reset stop. Allocations and shared leases stay owned. */
unsigned a733_npu_application_stopped(void);
#ifdef SUNSTONE_NPU_FINITE_RETURN
/* Finite single-core client after successful CLOSE/local stop. Suspend the
 * worker/bootstrap, park the idle PMIC task, check slots/borrows/mutexes and
 * observe disabled/inactive/clear NPU INTID97. Failure retains the session. */
unsigned a733_npu_application_quiesce(void);
unsigned a733_npu_application_quiescent(void);
#endif
/* Bounded acquire reads; latches any shared clock/registry/lock failure. */
unsigned a733_npu_application_health(void);
void a733_npu_application_fail(unsigned);
BaseType_t a733_npu_application_task_privileged(TaskFunction_t,void *);
/* Only this application's convolution object uses these wrappers. Every call
 * authenticates the actual worker and permanent parent before native I/O. */
extern const struct a733_npu_ops a733_npu_application_operations;

struct a733_observe_identity;
int a733_npu_application_observer_admit(void);
int a733_npu_application_request(const uint8_t[64],const struct a733_observe_identity *,uint32_t *,uint8_t[32]);
int a733_npu_application_frame(uint8_t[1024],const struct a733_observe_identity *,const uint8_t[32],const uint8_t[32]);
#endif
