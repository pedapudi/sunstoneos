/* Input and state of the returning SD0 read image (main.c). The host packs
 * the input with tools/storage-read-returning.py; the offsets below are part
 * of that wire format. */
#ifndef SUNSTONE_SDRT_H
#define SUNSTONE_SDRT_H
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include "a733_counter_clock.h"
#include "a733_platform_locks.h"
#include "a733_clock_leases.h"
#include "a733_pins_native.h"
#include "a733_gpu_resources_native.h"
#include "a733_rail_leases.h"
#include "a733_pmic_native.h"
#include "a733_mmc_platform_native.h"
#define SDRT_INPUT_ADDRESS UINT64_C(0x43000000)
#define SDRT_MAGIC UINT64_C(0x314e525445524453) /* "SDRETRN1" */
#define SDRT_VERSION 1u
#define SDRT_PLAN 256u
struct sdrt_read { uint32_t first, blocks; };
/* Configuration, not authority: the coordinator's signed approval of the
 * lifetime gates the input's creation on the host. */
struct sdrt_input {
    uint64_t magic, version, bytes, generation;
    uint64_t owner, handoff, topology;
    uint32_t profile, voltage_mask, reads, reserved;
    uint32_t fabric[3], fabric_hz[3];
    struct a733_pin_config pmic_pins[2], expected[7], desired[7];
    struct sdrt_read plan[SDRT_PLAN];
};
_Static_assert(offsetof(struct sdrt_input,profile)==56,"profile wire offset");
_Static_assert(offsetof(struct sdrt_input,fabric)==72,"fabric wire offset");
_Static_assert(offsetof(struct sdrt_input,pmic_pins)==96,"pin wire offset");
_Static_assert(offsetof(struct sdrt_input,plan)==144,"plan wire offset");
_Static_assert(sizeof(struct sdrt_input)==2192,"input wire size");
enum sdrt_stage { SDRT_CONSTRUCT, SDRT_IDENTIFY, SDRT_CARD, SDRT_READ, SDRT_POLL, SDRT_TAKE };
struct sdrt {
    struct sdrt_input input;
    struct a733_counter_clock counter;
    struct a733_platform_locks locks;
    struct a733_clock_leases clocks;
    struct a733_pins_native pin_native;
    struct a733_pin_handoff handoff;
    struct a733_pins pins;
    struct a733_gpu_resource_native gpu_native;
    struct a733_gpu_resource_manager gpu;
    struct a733_rail_leases rails;
    struct a733_pmic_native pmic_native;
    struct a733_pmic_config pmic_config;
    struct a733_pmic pmic;
    struct a733_mmc_platform_native native;
    struct a733_mmc_platform_config config;
    struct a733_mmc_platform platform;
    struct sunxi_mmc_card card;
    struct sunxi_mmc_result result;
    atomic_uint failed, pmic_stop, pmic_parked;
    uint64_t start, end, started, identified_us;
    unsigned stage, done, reason, card_valid;
    uint32_t elapsed[SDRT_PLAN], crc[SDRT_PLAN];
    uint8_t first[512];
};
extern struct sdrt sdrt;
int sdrt_setup(void);
int sdrt_step(void);
int sdrt_quiet(void);
void sdrt_report(int outcome);
uint64_t sdrt_ticks(void);
#endif
