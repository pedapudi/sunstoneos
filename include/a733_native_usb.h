#ifndef SUNSTONE_A733_NATIVE_USB_H
#define SUNSTONE_A733_NATIVE_USB_H
#include <stdatomic.h>
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_pmic_native.h"
#include "a733_pins_native.h"
#include "a733_usb_platform.h"
#include "a733_gpu_resources_native.h"
struct a733_native_usb_config {
    uint64_t identity;
    struct a733_pmic_config pmic;
    struct a733_pin_handoff pins;
    struct a733_usb_platform_config usb;
    uint32_t usb_supply_uv[4];
};
/* One permanent owner. The application retains admission and failure policy. */
struct a733_native_usb {
    struct a733_native_usb_config boot;
    struct a733_counter_clock counter;
    struct a733_platform_locks locks;
    struct a733_clock_leases clocks;
    struct a733_pins_native pin_native;
    struct a733_pins pins;
    struct a733_gpu_resource_native gpu_native;
    struct a733_gpu_resource_manager gpu;
    struct a733_rail_leases rails;
    struct a733_pmic_native pmic_native;
    struct a733_pmic pmic;
    struct a733_usb_platform usb;
    struct a733_rail_token usb_rail;
    struct a733_pmic_lifetime usb_power;
    struct a733_pmic_request usb_request[4];
    struct a733_pmic_snapshot usb_snapshot[4];
    TaskHandle_t pmic_task,usb_task;
    int (*live)(void *);
    void (*fail)(void *);
    void *context;
    atomic_uint pmic_ready,usb_prepared;
    unsigned used,usb_step,usb_rail_index;
    /* First non-BUSY callback denial; no extra register reads. */
    unsigned usb_io_denial;
    int usb_io_provider_status;
    unsigned usb_provider_denial;
    int usb_provider_result;
};
enum a733_native_usb_provider_denial { A733_NATIVE_USB_PROVIDER_NONE,
    A733_NATIVE_USB_PROVIDER_LIVE, A733_NATIVE_USB_PROVIDER_TASK,
    A733_NATIVE_USB_PROVIDER_RETAINED_LOCK, A733_NATIVE_USB_PROVIDER_RAIL_INDEX,
    A733_NATIVE_USB_PROVIDER_CLOCK_OWNER, A733_NATIVE_USB_PROVIDER_RAIL_OWNER,
    A733_NATIVE_USB_PROVIDER_CLOCK, A733_NATIVE_USB_PROVIDER_RAIL,
    A733_NATIVE_USB_PROVIDER_PMIC };
enum a733_native_usb_denial { A733_NATIVE_USB_DENIAL_NONE,
    A733_NATIVE_USB_DENIAL_LIVE, A733_NATIVE_USB_DENIAL_TASK,
    A733_NATIVE_USB_DENIAL_MUTEX, A733_NATIVE_USB_DENIAL_PROVIDER,
    A733_NATIVE_USB_DENIAL_ADAPTER };
int a733_native_usb_init(struct a733_native_usb *,const struct a733_native_usb_config *,
    int (*)(void *),void (*)(void *),void *);
int a733_native_usb_bind(struct a733_native_usb *,TaskHandle_t,TaskHandle_t);
int a733_native_usb_step(struct a733_native_usb *);
int a733_native_usb_health(struct a733_native_usb *);
#define A733_NATIVE_USB_PCTL UINT32_C(0x04100040)
/* Store the PCTL byte of the packed word at USB0+0x40. Accepts only a word
 * equal to the current word with SoftConn (bit 6) clear; returns 1 after one
 * byte store, 0 without a store otherwise. The caller verifies the bit later. */
int a733_native_usb_pctl_write(uint32_t word);
#endif
