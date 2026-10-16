#ifndef SUNSTONE_A733_CLOCK_LEASES_H
#define SUNSTONE_A733_CLOCK_LEASES_H
#include <stdatomic.h>
#include <stdint.h>

#define A733_CLOCK_CLIENTS 32u
#define A733_CLOCK_OSC       (1u<<0)
#define A733_CLOCK_REF       (1u<<1)
#define A733_CLOCK_PERI0     (1u<<2)
#define A733_CLOCK_AUDIO1    (1u<<3)
#define A733_CLOCK_AUDIO2    (1u<<4) /* AUDIO1 DIV2 output. */
#define A733_CLOCK_AUDIO5    (1u<<5) /* AUDIO1 DIV5 output. */
#define A733_CLOCK_I2S0      (1u<<6)
#define A733_CLOCK_I2S1      (1u<<7)
#define A733_CLOCK_I2S2      (1u<<8)
#define A733_CLOCK_I2S3      (1u<<9)
#define A733_CLOCK_I2S4      (1u<<10)
#define A733_CLOCK_ASRC     (1u<<11)
#define A733_CLOCK_OWA_TX   (1u<<12)
#define A733_CLOCK_DMIC     (1u<<13)
#define A733_CLOCK_FABRIC   (1u<<14) /* AHB/NSI/MBUS source configuration and global gates. */
#define A733_CLOCK_DMA0     (1u<<15) /* Controller, own bus/MBUS leaf gates, reset/routes; exclusive. */
#define A733_CLOCK_PLL_NPU  (1u<<16) /* Also selectable by MBUS, DRAM0 and video clocks. */
#define A733_CLOCK_NPU      (1u<<17) /* Module, own bus/leaf gates, resets and PCK domain 4; exclusive. */
#define A733_POWER_DCDC2    (1u<<18) /* Stable rail or exclusive change authority; no clock parent. */
#define A733_CLOCK_LOCALS  UINT32_C(0x3fc0)
#define A733_CLOCK_ALL     UINT32_C(0x7ffff)
#define A733_CLOCK_AUDIO_CHANGE (A733_CLOCK_AUDIO1|A733_CLOCK_AUDIO2|A733_CLOCK_AUDIO5|A733_CLOCK_LOCALS)

enum a733_clock_result { A733_CLOCK_OK, A733_CLOCK_BUSY, A733_CLOCK_INVALID,
    A733_CLOCK_EXPIRED, A733_CLOCK_FAULT, A733_CLOCK_EXHAUSTED };
struct a733_clock_token { uint64_t id, owner; uint32_t shared, exclusive; };
struct a733_clock_leases {
    atomic_uint busy;
    uint64_t next_id, last_now;
    struct a733_clock_token slots[A733_CLOCK_CLIENTS];
    int (*now)(void *, uint64_t *);
    void *context;
    uint32_t initialized, failed, audio_profile;
};
/* Private platform object, zero once and initialize before concurrent use.
 * now returns exactly 1, uses monotonic microseconds, and must be bounded and
 * non-reentrant. Calls use a nonblocking atomic lock; BUSY never spins.
 * No MMIO, scheduler, allocation, or hardware-state discovery occurs here.
 * Every relevant clock writer must use this ONE registry plus the common
 * register mutex. Legacy resource managers must be bound before coexistence.
 */
int a733_clock_leases_init(struct a733_clock_leases *, int (*now)(void *,uint64_t *), void *);
/* Privileged owner IDs are trusted registrations, not application input.
 * Shared clock dependencies expand through AUDIO1/PERI0/PLL_NPU -> REF -> OSC.
 * Changing a child holds its ancestors stable. Local controller scopes are
 * exclusive only; their selected parent must be leased separately. I2S/ASRC/
 * OWA/DMIC local ownership includes module clock, gate/reset and producer/FIFO
 * route writers. Pins, voltage, DMA channels and memory are separate leases.
 * FABRIC stability does not imply parent stability: callers must also lease
 * each selected PLL/reference parent. DMA0 requires a platform handoff covering
 * all channels and remote-MCU/routing writers; idle bits cannot establish it.
 * PLL_NPU retuning requires exclusion of every parent consumer, including
 * memory/fabric consumers. NPU local ownership alone cannot authorize it.
 * Stable PCK clock and DCDC2 rail ownership remain separate platform leases.
 * DCDC2 shared/exclusive rail authority also uses this registry. Every PMIC
 * voltage/enable writer must acquire its exclusive scope; a mutex alone only
 * serializes transactions. DCDC2 has no clock ancestors or audio-profile effect.
 * Granting a token precedes all caller hardware operations. An uncertain
 * operation requires retain(), never release(). All refusal outputs unchanged.
 */
enum a733_clock_result a733_clock_acquire(struct a733_clock_leases *, uint64_t owner,
    uint32_t shared, uint32_t exclusive, uint64_t deadline, struct a733_clock_token *);
/* Release only after hardware quiescence/restoration is proved by the owner.
 * Exact token authentication prevents stale or modified releases. The supplied
 * token is immutable. Deadline refusal retains the registry reservation.
 */
enum a733_clock_result a733_clock_release(struct a733_clock_leases *,
    const struct a733_clock_token *, uint64_t deadline);
/* Called only by the trusted audio parent after verifying the active N=128,
 * DIV5=5 profile, output/PLL gates and disabled modulation/auto-gating while
 * holding AUDIO_CHANGE. This atomically publishes profile 1 and replaces the
 * change guard with stable DIV5 ownership (including parent dependencies).
 * A fresh token invalidates the old identity; no interval admits a consumer
 * before profile publication. It does not itself read or program the PLL.
 */
enum a733_clock_result a733_clock_publish_audio(struct a733_clock_leases *,
    const struct a733_clock_token *, uint64_t deadline, struct a733_clock_token *);
/* Read-only, nonblocking observation under the registry lock. OK requires an
 * exact live token in a healthy registry; BUSY admits no conclusion. No time
 * sample, state change, token release, or hardware access occurs. */
enum a733_clock_result a733_clock_token_status(struct a733_clock_leases *,const struct a733_clock_token *);
/* Retain all reservations and prohibit later acquisition/release/publication.
 * BUSY means the caller must retry this metadata operation before relinquishing
 * its existing exclusive ownership. No physical action or implicit reset. */
enum a733_clock_result a733_clock_retain(struct a733_clock_leases *, const struct a733_clock_token *);
#endif
