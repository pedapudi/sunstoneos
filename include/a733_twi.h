#ifndef SUNSTONE_A733_TWI_H
#define SUNSTONE_A733_TWI_H
#include "a733_clock_leases.h"
#include "a733_pins.h"
#include "a733_rail_leases.h"

/* CAM1 uses main TWI11, PE7/PE8 function8, and a seven-bit SCCB address.
 * This engine transfers one data byte at a one- or two-byte register address.
 * CAM1 uses the two-byte big-endian register form.
 * It never changes the slave address into an eight-bit API convention. */
#define A733_TWI11_BASE UINT32_C(0x0251b000)
#define A733_TWI11_BGR UINT32_C(0x02002eac)
enum a733_twi_controller { A733_TWI_MAIN11,A733_TWI_R1 };
enum a733_twi_state { A733_TWI_NEW, A733_TWI_READY, A733_TWI_BEGIN,
    A733_TWI_SEND, A733_TWI_WAIT, A733_TWI_STOP, A733_TWI_STOP_WAIT,
    A733_TWI_COMPLETE, A733_TWI_FAULT };
struct a733_twi_ops {
    int (*now)(void *,uint64_t *);
    /* Real ownership authentication: 1 owned, 0 metadata BUSY, -1 refused.
     * enter acquires the platform I2C mutex for the entire transaction.
     * Authentication while held must not acquire the register mutex. */
    int (*authenticate)(void *);
    int (*enter)(void *,uint64_t); /* 0 busy without acquisition,1 held,-1 fault. */
    void (*leave)(void *);
    void (*retain)(void *);
    int (*read)(void *,uintptr_t,uint32_t *);
    int (*write)(void *,uintptr_t,uint32_t);
    int (*current)(void *); /* Designated serialized task, independent of authority health. */
};
struct a733_twi {
    struct a733_twi *self;
    struct a733_twi_ops ops;
    void *context;
    uintptr_t base;
    uint64_t deadline,last_now;
    uint32_t divider,last_status;
    uint16_t reg;
    uint8_t address,value,received,index,phase,reading,held,entered,register_bytes;
    uint8_t block[40],length;
    enum a733_twi_state state;
};
/* Zero private storage, one serialized task, immutable disjoint arguments.
 * TWI11 is already initialized to standard mode with this divider, with no
 * alternate driver/DMA/IRQ owner. The native prepare operation establishes
 * these register conditions; this initializer performs no hardware access. */
int a733_twi_init(struct a733_twi *,const struct a733_twi_ops *,void *,uint32_t divider);
/* Same byte engine for FUSB302 on S_TWI1, whose register map is identical to
 * TWI0 (manual18.11.7). Closed controller selection fixes0x0251b000/0x07084000;
 * address width is1 or2 bytes. Native S_TWI1 ownership/clock/pins and exclusion
 * of autonomous driver/DMA writers belong to its own platform adapter. */
int a733_twi_init_controller(struct a733_twi *,const struct a733_twi_ops *,void *,
    uint32_t divider,enum a733_twi_controller,unsigned register_bytes);
/* Choose the fastest source-defined 50% duty divider at or below100kHz.
 * input_hz is the observed, leased APB1 rate. No source or bus reconfiguration. */
int a733_twi_divider(uint32_t input_hz,uint32_t *divider,uint32_t *rate_hz);
int a733_twi_request(struct a733_twi *,uint8_t address,uint16_t reg,
    unsigned reading,uint8_t value,uint64_t deadline);
/* Bounded FIFO transfer in one I2C transaction, with a repeated START for reads.
 * Write input is copied before effects. Read output is copied only by take_block.
 * Buffers must be disjoint from private engine storage. Maximum 40 bytes. */
int a733_twi_request_block(struct a733_twi *,uint8_t,uint16_t,unsigned,
    const uint8_t *,unsigned,uint64_t);
int a733_twi_take_block(struct a733_twi *,uint8_t *,unsigned);
/* One bounded protocol step; 0 pending,1 complete,-1 terminal retained fault.
 * NACK/arbitration/error/deadline/MMIO uncertainty retains the bus and every
 * lease. No STOP after an unexpected status, no retry, reset or bus pulses.
 * Authentication BUSY before a step's first access leaves it pending; after an
 * access, the observation is repeated until the deadline without replay.
 * Successful read data stays private until take() authenticates ownership. */
int a733_twi_poll(struct a733_twi *);
int a733_twi_take(struct a733_twi *,uint8_t *value);
/* Designated task only, outside another engine call. Terminally retain the
 * current transaction on upstream revocation without STOP/reset/retry/unlock.
 * Repeated calls after FAULT perform no callbacks. A wrong task is refused
 * without altering the owning task's transaction. */
int a733_twi_retain(struct a733_twi *);

struct a733_platform_locks;
struct a733_twi_native_config {
    struct a733_clock_leases *clocks;
    struct a733_clock_token clock;
    struct a733_pins *pins;
    struct a733_pin_lease_banked pin;
    struct a733_rail_leases *rails;
    const struct a733_rail_borrow *rail; /* Actual pinned parent borrow. */
    struct a733_platform_locks *locks;
    void *task;
    uint32_t apb1,apb1_hz;
};
struct a733_twi_native {
    struct a733_twi_native *self;
    struct a733_twi_native_config config;
    struct a733_twi_ops operations;
    uint32_t divider,rate_hz;
    uint64_t deadline,until; /* Prepare deadline; current transaction deadline. */
    unsigned prepared,held,fault,stored,busy;
};
/* One native TWI11 instance, constructed once during serialized platform boot.
 * Its real token has exclusive TWI11 and stable FABRIC/REF/OSC (plus PERI0
 * when selected). The private pin token includes PE7/PE8 function8; the rail
 * borrow stays pinned. No caller may release or mutate these borrowed objects.
 * Qualified boot ownership must exclude remote writers, IRQ/DMA/driver mode,
 * establish reset safety, Device mappings/security, electrical pullups/bank
 * voltage, and independent recovery. This function cannot discover that proof.
 * No success callback substitutes these prerequisites; native access is direct.
 */
int a733_twi_native_init(struct a733_twi_native *,const struct a733_twi_native_config *);
/* Designated EL1 FreeRTOS task; actual common register mutex. The initial local
 * gate/reset must both be0. Release reset, then open gate as prescribed by the
 * manual, establish100kHz-or-less standard mode, and verify inactive driver,
 * IRQ and high bus lines. Failed/uncertain writes retain the mutex and token.
 * Registry or ledger BUSY before the first write leaves the mutex and returns
 * 0; after it, the observation is repeated until the deadline. Transactions
 * likewise repeat a BUSY observation in place and never reissue an access.
 * Already enabled/inherited controllers are refused rather than reset. */
int a733_twi_native_prepare(struct a733_twi_native *,uint64_t deadline);
#endif
