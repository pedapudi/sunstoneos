#ifndef SUNSTONE_A733_STORAGE_UPDATE_NATIVE_H
#define SUNSTONE_A733_STORAGE_UPDATE_NATIVE_H
#include "a733_storage_update.h"
#include <stdatomic.h>
#include "a733_usb_platform.h"
#include "a733_maintenance_watchdog.h"
/* Owned by the dedicated TinyUSB task. Every field is atomic so readers can
 * obtain a stable version without a C data race. Only actual completed OUT/IN
 * exchanges advance these records. A bus loss is permanent after admission. */
struct a733_storage_native_transport {
    atomic_uint_fast64_t version,generation,accepted_sequence,accepted_hash[4];
    atomic_uint_fast64_t accepted_command,response_sequence,completed_sequence;
    atomic_uint_fast64_t connected,lost;
    struct a733_storage_channel *channel;
    const struct a733_storage_update *service;
    void *owner_task;
};
struct a733_maintenance_lease;
/* Privileged bootstrap supplies this complete, immutable record before task
 * creation. USB cannot create or modify it. The host must establish physical
 * topology, producer exclusion, Device mappings, cold controllers and recovery
 * before entry. Artifact hashes identify that evidence; they do not prove it.
 * In particular, USB supply wiring follows the reviewed detailed schematic.
 * No qualified physical launch contract is currently available. */
struct a733_storage_native_boot {
    uint64_t identity,topology;
    uint8_t topology_artifact[32],launch_artifact[32]; /* launch_artifact: transferred image SHA256. */
    struct a733_storage_grant grant;
    struct a733_mmc_platform_config mmc;
    struct a733_pmic_config pmic;
    struct a733_pin_handoff pins;
    struct a733_usb_platform_config usb;
    /* Qualified programmed targets in DCDC1, DC1SW2, DCDC9, ELDO6 order.
     * The source decoder's supported range does not establish safe operation. */
    uint32_t usb_supply_uv[4];
};
#ifdef A733_STORAGE_NATIVE_IMPLEMENTATION
#include "a733_native_usb.h"
#include "a733_platform_locks.h"
#include "a733_pmic_native.h"
#include "a733_pins_native.h"
#include "a733_mmc_platform_native.h"
/* Private permanent composition. Only the storage target translation units may
 * access this storage. No task deletion, reinitialization or release exists. */
struct a733_storage_native {
    struct a733_storage_native_boot boot;
    struct a733_native_usb providers;
    struct a733_mmc_platform_native mmc_native;
    struct a733_mmc_platform mmc;
    struct a733_storage_update service;
    struct a733_storage_channel channel;
    struct a733_storage_native_transport transport;
    struct a733_maintenance_lease *maintenance;
    struct a733_maintenance_transfer transfer;
    StaticTask_t task[3];
    StackType_t stack[3][2048];
    TaskHandle_t task_handle[3];
    atomic_uint active,failed,service_ready;
    unsigned used;
};
#else
struct a733_storage_native;
#endif
/* Target-local API. Call once before starting the scheduler. A real armed
 * maintenance lease is mandatory; null or foreign leases refuse activation. */
int a733_storage_native_start(const struct a733_storage_native_boot *,struct a733_maintenance_lease *,
    const struct a733_maintenance_transfer *);
int a733_storage_native_task_privileged(void (*)(void *),void *);
int a733_storage_native_live(struct a733_storage_native *);
void a733_storage_native_fail(struct a733_storage_native *);
void a733_storage_native_pmic_task(void *);
void a733_storage_native_worker_task(void *);
void a733_storage_native_usb_task(void *);
int a733_storage_native_usb_step(struct a733_storage_native *);
/* USB task only, after PREPARED publication. 1 admits one bounded controller
 * call, 0 waits without access, -1 retains failure. No token is released. */
int a733_storage_native_usb_health(struct a733_storage_native *);
#endif
