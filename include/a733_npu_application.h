#ifndef SUNSTONE_A733_NPU_APPLICATION_H
#define SUNSTONE_A733_NPU_APPLICATION_H
#include "a733_npu_reserved.h"
#include "a733_pmic_native.h"
#include "a733_pins_native.h"

#define A733_NPU_APPLICATION_COMMAND UINT32_C(0x33)
#define A733_NPU_APPLICATION_FRAME UINT64_C(0x415337334e505531)
#define A733_NPU_APPLICATION_USB_REQUIRED UINT64_C(15)
#define A733_NPU_APPLICATION_USB_SUPERVISED UINT64_C(23)

/* Immutable input supplied by a separately qualified board admission. USB bits
 * 0..3 cover exclusive USB0/PHY/whole CCU words, Device mappings, remote-owner
 * exclusion and independent watchdog/ROM recovery. NPU flags retain the existing
 * startup meaning, including complete SRAM ownership and effective confinement.
 * These identities and flags cannot establish any physical fact themselves. */
struct a733_npu_application_profile {
    uint64_t epoch,generation,lease,npu_evidence,command_admission,usb_evidence;
    uint64_t pmic_evidence,pin_evidence,npu_owner,pmic_owner,usb_owner,bridge_owner;
    uint64_t npu_established,usb_established;
};
struct a733_npu_application_boot {
    struct a733_npu_application_profile profile;
    /* Deadlines must be zero in this input. Initialization derives actual
     * bounded deadlines from the authenticated boot contract's counter value. */
    struct a733_npu_platform_config npu;
    struct a733_pmic_config pmic;
    struct a733_pin_handoff pins;
};
extern const struct a733_npu_application_boot a733_npu_application_boot;
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
    unsigned char first_digest[32];
    unsigned char output[NPU_CONVOLUTION_IO_CAPACITY];
};
extern struct a733_npu_application_result a733_npu_application_result;
extern atomic_uint a733_npu_application_failed;

int a733_npu_application_init(TaskHandle_t client);
struct npu_convolution *a733_npu_application_client(void);
unsigned a733_npu_application_ready(void);
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
