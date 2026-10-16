#ifndef SUNSTONE_A733_NPU_STARTUP_H
#define SUNSTONE_A733_NPU_STARTUP_H
#include "a733_counter_clock.h"
#include "a733_npu_platform.h"
#include "a733_npu_pll.h"
#include "a733_platform_locks.h"
#include "npu_service.h"

#define A733_NPU_HANDOFF_QUIET       (1u<<0)
#define A733_NPU_HANDOFF_SRAM_FREE   (1u<<1)
#define A733_NPU_HANDOFF_IRQ_MASKED  (1u<<2)
#define A733_NPU_HANDOFF_RECOVERY    (1u<<3)
#define A733_NPU_HANDOFF_REMOTE      (1u<<4)
#define A733_NPU_HANDOFF_I2C         (1u<<5)
#define A733_NPU_HANDOFF_MEMORY      (1u<<6)
#define A733_NPU_HANDOFF_REQUIRED    UINT32_C(0x7f)
#define A733_NPU_RAIL_SETTLE_US UINT64_C(5160)
/* One global GPU/PCK manager, shared with GPU clients and all legacy change
 * guards. Wrappers bind its operations to the common physical counter and
 * register/PMIC mutexes. Never instantiate a second manager for the same PCK.
 * Native operations preserve existing GPU resource behavior. */
struct a733_npu_startup_resources {
    struct a733_gpu_resource_manager manager;
    struct a733_gpu_resource_native native;
    struct a733_counter_clock *clock;
    struct a733_platform_locks *locks;
    unsigned initialized;
};
/* Copy private authenticated evidence before scheduling. Flags describe prior
 * independently established proof, including remote/secure firmware exclusion,
 * NPU/shared-SRAM quiescence, secure attribution, inherited I2C clocks/pins,
 * reserved backend/command/MMU memory and independent watchdog recovery.
 * Neither flags nor numeric evidence IDs establish those physical facts.
 * Command admission remains an independent prerequisite. Startup authenticates
 * exactly the supplied backend/image/descriptor table, without parsing commands.
 */
struct a733_npu_handoff {
    struct a733_npu_platform_config allowed;
    struct a733_npu_ops backend;
    void *backend_cookie;
    struct a733_npu_mmu_image image;
    struct a733_npu_command commands[NPU_SERVICE_COMMANDS];
    uintptr_t controller;
    uint64_t evidence, command_admission;
    uint32_t established;
    unsigned command_count, pck_client;
};
struct a733_npu_startup {
    struct a733_npu_handoff handoff;
    struct a733_npu_platform platform;
    struct a733_npu_platform_operations operations;
    struct a733_clock_token clock_token,rail_token;
    struct a733_pmic_consumer pmic;
    struct a733_npu_pll *pll_source;
    const struct a733_npu_startup *pll_binding_self;
    struct a733_npu_pll_config pll_config;
    struct a733_npu_pll_operations pll_operations;
    void *pll_cookie;
    unsigned pll_transferred;
    uint64_t pll_whole_platform;
    struct a733_gpu_resource_lease pck_token;
    struct a733_npu_startup_resources *shared;
    struct a733_clock_leases *registry;
    struct a733_i2c bus;
    TaskHandle_t task;
    uint64_t settle_until;
    unsigned initialized,failed,stage,register_held,i2c_held,rail_step;
    uint8_t rail[6];
};
/* EL1, pre-scheduler. Shared counter must already be initialized (24 MHz on
 * A733); locks and registry use that same object. No hardware acquisition.
 * Resource initializer calls the native adapter's counter-frequency check only.
 * All objects are private, disjoint, statically reserved for their lifetimes. */
int a733_npu_startup_resources_init(struct a733_npu_startup_resources *,
    struct a733_counter_clock *,struct a733_platform_locks *);
int a733_npu_startup_init(struct a733_npu_startup *,const struct a733_npu_handoff *,
    struct a733_npu_startup_resources *,struct a733_clock_leases *);
/* Shared runtime mode, EL1 before scheduling and before any manager acquisition.
 * Disables the shared native GPU legacy reader even without a GPU view. Both
 * consumers must bind this same provider/ledger/global manager. NPU owns a
 * stable DCDC2 parent and keeps the final observation ticket for service life.
 * An unbound startup is incompatible with a shared-provider native adapter. */
int a733_npu_startup_bind_pmic(struct a733_npu_startup *,struct a733_pmic *,
    const struct a733_pmic_lifetime *);
/* Optional, EL1 pre-scheduler binding before any startup poll. Source registry,
 * owner, handoff and fabric must match. The source may already be prepared by
 * cold boot, or the designated bootstrap task must serialize source polling
 * with startup polling. No concurrent observer/writer of either object is
 * permitted; their state fields are plain C data. One source has one successor.
 * Source, all callback metadata and its whole-platform reservation MUST remain
 * live and immutable for startup/service lifetime. Neither object may be copied
 * or moved. The source provider still has to establish real boot/remote/RAM
 * authority; binding does not infer those facts from metadata.
 * Bootstrap polling atomically publishes stable PLL_NPU ownership and adopts
 * the fresh token while preserving exclusive local NPU and all parent scopes.
 * This does not bypass rail, PCK, local platform or service admission. */
int a733_npu_startup_bind_pll(struct a733_npu_startup *,struct a733_npu_pll *);
int a733_npu_startup_bind_task(struct a733_npu_startup *,TaskHandle_t);
/* One designated EL1 bootstrap task after scheduler start. Acquisition and rail
 * snapshots never nest platform mutexes. Failure retains every admitted lease;
 * uncertain I/O retains its mutex and blocks later shared transactions.
 * Shared mode queues actual asynchronous observations on the provider task.
 * Isolated legacy I2C has its own bounded physical-counter timeout; startup
 * checks its absolute deadline before and after each transaction.
 * No PMIC data write, bus reset, automatic retry or resource release occurs. */
enum a733_npu_platform_result a733_npu_startup_poll(struct a733_npu_startup *);
int a733_npu_startup_ready(const struct a733_npu_startup *);
/* npu_service_activation.prepared: read-only metadata/health observation,
 * exact backend/image/command comparison, task authority and all live tokens.
 * Returns 1 ready, 0 pending/metadata-lock contention, -1 retained failure.
 * Does not touch MMIO/I2C or authorize arbitrary command semantics. */
int a733_npu_startup_prepared(void *,const struct a733_npu_ops *,void *,
    const struct a733_npu_mmu_image *,const struct a733_npu_command *,unsigned);
#endif
