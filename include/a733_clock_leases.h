#ifndef SUNSTONE_A733_CLOCK_LEASES_H
#define SUNSTONE_A733_CLOCK_LEASES_H
#include <stdatomic.h>
#include <stdint.h>

typedef uint64_t a733_clock_mask_t;

/* 128 simultaneous reservations: space for one owner of each of 64 possible
 * scopes plus 64 additional shared/coordination leases. Exhaustion returns BUSY.
 * The slot array occupies 4096 bytes; no dynamic allocation occurs.
 * Tokens are private runtime metadata, not a persistent or wire ABI. All
 * producers and consumers must be rebuilt together when this layout changes. */
#define A733_CLOCK_CLIENTS 128u
#define A733_CLOCK_OSC       (UINT64_C(1)<<0)
#define A733_CLOCK_REF       (UINT64_C(1)<<1)
#define A733_CLOCK_PERI0     (UINT64_C(1)<<2)
#define A733_CLOCK_AUDIO1    (UINT64_C(1)<<3)
#define A733_CLOCK_AUDIO2    (UINT64_C(1)<<4) /* AUDIO1 DIV2 output. */
#define A733_CLOCK_AUDIO5    (UINT64_C(1)<<5) /* AUDIO1 DIV5 output. */
#define A733_CLOCK_I2S0      (UINT64_C(1)<<6)
#define A733_CLOCK_I2S1      (UINT64_C(1)<<7)
#define A733_CLOCK_I2S2      (UINT64_C(1)<<8)
#define A733_CLOCK_I2S3      (UINT64_C(1)<<9)
#define A733_CLOCK_I2S4      (UINT64_C(1)<<10)
#define A733_CLOCK_ASRC     (UINT64_C(1)<<11)
#define A733_CLOCK_OWA_TX   (UINT64_C(1)<<12)
#define A733_CLOCK_DMIC     (UINT64_C(1)<<13)
#define A733_CLOCK_FABRIC   (UINT64_C(1)<<14) /* AHB/NSI/MBUS/APB1 sources, divisors and global gates. */
#define A733_CLOCK_DMA0     (UINT64_C(1)<<15) /* Controller, own bus/MBUS leaf gates, reset/routes; exclusive. */
#define A733_CLOCK_PLL_NPU  (UINT64_C(1)<<16) /* Also selectable by MBUS, DRAM0 and video clocks. */
#define A733_CLOCK_NPU      (UINT64_C(1)<<17) /* Module, own bus/leaf gates, resets and PCK domain 4; exclusive. */
#define A733_POWER_DCDC2    (UINT64_C(1)<<18) /* Stable rail or exclusive change authority; no clock parent. */
#define A733_CLOCK_PLL_VIDEO0 (UINT64_C(1)<<19)
#define A733_CLOCK_PLL_DE     (UINT64_C(1)<<20)
#define A733_CLOCK_VI         (UINT64_C(1)<<21) /* All CSI/ISP/analog/MCLK and VI domain controls. */
#define A733_CLOCK_DE_SYS     (UINT64_C(1)<<22) /* Both DE outputs, DMA, routes, SRAM and domain. */
#define A733_CLOCK_VO0        (UINT64_C(1)<<23) /* VO0 shared words, LCD/DSI/LVDS and domain. */
#define A733_CLOCK_IOMMU_SYS  (UINT64_C(1)<<24) /* Both banks; master exclusion remains separate. */
#define A733_CLOCK_PLL_VE0    (UINT64_C(1)<<25)
#define A733_CLOCK_PLL_VE1    (UINT64_C(1)<<26)
#define A733_CLOCK_VE_DEC     (UINT64_C(1)<<27) /* Decoder clocks, reset, routes and domain 2. */
#define A733_CLOCK_DISPLL1    (UINT64_C(1)<<28) /* Local display PLL1, DCXO parent. */
#define A733_CLOCK_THS        (UINT64_C(1)<<29) /* THS controller, bus gate/reset and IRQ; exclusive. */
#define A733_CLOCK_GPADC24M   (UINT64_C(1)<<30) /* Shared ADC clock word and all its source/gate writers. */
#define A733_CLOCK_APB_UART   (UINT64_C(1)<<31) /* Main UART source/divider, CCU+0x538. */
#define A733_CLOCK_APBS1      (UINT64_C(1)<<32) /* S UART/TWI source/divider, R_CCU+0x10. */
#define A733_CLOCK_UART0      (UINT64_C(1)<<33)
#define A733_CLOCK_UART1      (UINT64_C(1)<<34)
#define A733_CLOCK_UART2      (UINT64_C(1)<<35)
#define A733_CLOCK_UART3      (UINT64_C(1)<<36)
#define A733_CLOCK_UART4      (UINT64_C(1)<<37)
#define A733_CLOCK_UART5      (UINT64_C(1)<<38)
#define A733_CLOCK_UART6      (UINT64_C(1)<<39)
#define A733_CLOCK_S_UART0    (UINT64_C(1)<<40)
#define A733_CLOCK_S_UART1    (UINT64_C(1)<<41)
#define A733_CLOCK_SPI0       (UINT64_C(1)<<42)
#define A733_CLOCK_SPI1       (UINT64_C(1)<<43)
#define A733_CLOCK_SPI2       (UINT64_C(1)<<44)
#define A733_CLOCK_SPI3       (UINT64_C(1)<<45)
#define A733_CLOCK_SPI4       (UINT64_C(1)<<46)
#define A733_CLOCK_S_SPI      (UINT64_C(1)<<47)
#define A733_CLOCK_CE         (UINT64_C(1)<<48) /* Complete secure/nonsecure CE controller. */
#define A733_CLOCK_RC16M      (UINT64_C(1)<<49) /* Independent internal oscillator; no PLL parent. */
#define A733_CLOCK_USB01_REF  (UINT64_C(1)<<50) /* USB0/1 reference mux/gate at CCU+0x1340. */
#define A733_CLOCK_USB0       (UINT64_C(1)<<51) /* Complete USB0 port; exclusive. */
#define A733_CLOCK_USB1       (UINT64_C(1)<<52) /* Complete USB1 port; exclusive. */
#define A733_CLOCK_SDC1       (UINT64_C(1)<<53) /* Complete SDC1 host and dedicated clock/reset. */
#define A733_CLOCK_RADIO_MODULE (UINT64_C(1)<<54) /* Shared Wi-Fi/BT module lifecycle; exclusive. */
#define A733_CLOCK_S_TWI0     (UINT64_C(1)<<55) /* Complete S_TWI0 including autonomous engines. */
#define A733_CLOCK_SDC0       (UINT64_C(1)<<56) /* Complete SDC0 host and dedicated clock/reset. */
#define A733_CLOCK_SDC2       (UINT64_C(1)<<57) /* Complete SDC2 host and dedicated clock/reset. */
#define A733_CLOCK_SDC3       (UINT64_C(1)<<58) /* Complete SDC3 eMMC host and dedicated clock/reset. */
#define A733_CLOCK_PCIE0      (UINT64_C(1)<<59) /* Local controller, clocks/reset, iATU, IRQ and eDMA; exclusive. */
#define A733_CLOCK_SERDES_COMMON (UINT64_C(1)<<60) /* Global clock/reset and shared SERDES fabric gates. */
#define A733_CLOCK_COMB1      (UINT64_C(1)<<61) /* PCIe/USB3 lane, PHY, pipe mux and reference gates; exclusive. */
#define A733_CLOCK_USB_PCIE_DOMAIN (UINT64_C(1)<<62) /* Shared stable PCK domain7; exclusive transitions. */
#define A733_CLOCK_VO1        (UINT64_C(1)<<63) /* Both TV paths, HDMI/eDP, TOP/PLL, CCU and PCK domain10; exclusive. */
#define A733_CLOCK_MAIN_UARTS (A733_CLOCK_UART0|A733_CLOCK_UART1|A733_CLOCK_UART2|A733_CLOCK_UART3|A733_CLOCK_UART4|A733_CLOCK_UART5|A733_CLOCK_UART6)
#define A733_CLOCK_S_UARTS (A733_CLOCK_S_UART0|A733_CLOCK_S_UART1)
#define A733_CLOCK_SPIS (A733_CLOCK_SPI0|A733_CLOCK_SPI1|A733_CLOCK_SPI2|A733_CLOCK_SPI3|A733_CLOCK_SPI4|A733_CLOCK_S_SPI)
#define A733_CLOCK_MEDIA_LOCALS (A733_CLOCK_VI|A733_CLOCK_DE_SYS|A733_CLOCK_VO0|A733_CLOCK_IOMMU_SYS|A733_CLOCK_VE_DEC|A733_CLOCK_VO1)
#define A733_CLOCK_LOCALS  UINT64_C(0x3fc0)
#define A733_CLOCK_ALL     UINT64_MAX /* All64 assigned scopes; VO1 is exclusive-only. */
#define A733_CLOCK_AUDIO_CHANGE (A733_CLOCK_AUDIO1|A733_CLOCK_AUDIO2|A733_CLOCK_AUDIO5|A733_CLOCK_LOCALS)

enum a733_clock_result { A733_CLOCK_OK, A733_CLOCK_BUSY, A733_CLOCK_INVALID,
    A733_CLOCK_EXPIRED, A733_CLOCK_FAULT, A733_CLOCK_EXHAUSTED };
struct a733_clock_token { uint64_t id, owner; a733_clock_mask_t shared, exclusive; };
struct a733_clock_leases {
    atomic_uint busy;
    uint64_t next_id, last_now;
    struct a733_clock_token slots[A733_CLOCK_CLIENTS];
    int (*now)(void *, uint64_t *);
    void *context;
    uint32_t initialized;
    atomic_uint failed; /* Acquire-load permitted under permanent exact leases. */
    uint32_t audio_profile;
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
 * Shared PLL dependencies expand through REF -> OSC, including VIDEO0, DE,
 * VE0 and VE1. AUDIO2/5 also depend on AUDIO1. VO0 depends on DISPLL1,
 * which depends directly on OSC. Parent change authority therefore conflicts
 * with every VO0 consumer; a cold coordinator can acquire both exclusively.
 * THS depends on GPADC24M; GPADC24M conservatively reserves REF -> OSC for
 * both SYS24M and DCXO selections. THS is exclusive-only. A GPADC24M change
 * requires exclusion of every ADC/thermal consumer; analog supply authority
 * and observed source frequency remain separate physical prerequisites.
 * Each main UART depends on APB_UART; each S UART depends on APBS1.
 * Both bus scopes conservatively reserve REF -> OSC for SYS24M/DCXO routes.
 * An actual PERI0 selection requires a separate verified PERI0 reservation;
 * unsupported oscillator selections cannot be admitted from a token alone.
 * APBS1 also feeds S_TWI0-2, whose consumers must participate before retuning.
 * RC16M owns its independent oscillator gate/trim/source writers, including
 * RTC+0x160 bit0. It has no OSC/REF ancestor. The shared RTC word also contains
 * DCXO/wake fields; every writer preserves unowned fields under the mutex.
 * CE sample-clock users reserve RC16M explicitly, in addition to their selected
 * module parent. Neither oscillator scope implies actual source/rate proof.
 * UART local scopes own one controller, its gate/reset/DMA route and GIC
 * interrupt. SPI local scopes own one controller and its module/bus/reset
 * fields. CE owns all secure/nonsecure queues, shared command/health controls
 * and its dedicated module/bus/reset/MBUS leaf fields. These scopes exclude
 * other local owners; SPI/CE selected parents and fabric are reserved explicitly.
 * Shared register words require the common mutex and preservation of other
 * controllers' fields. Pins, rail control and remote ownership remain separate.
 * USB0 and USB1 each own their complete controller/PHY/EHCI/OHCI port,
 * role routes, and local clock/reset fields at CCU+0x1300/1304 or +0x1308/130c.
 * Both reserve USB01_REF -> REF -> OSC. USB01_REF covers CCU+0x1340 mux26:24
 * and gate31; SYS24M/DCXO source selection still requires live rate proof.
 * USB2 is independent and is not authorized by these scopes. Upstream MSI-LITE2
 * gate/reset at CCU+0x5a4 and USB_SYS gate at +0x5c0 bit9 remain FABRIC
 * authority; a local port token grants no upstream mutation rights. A shared
 * FABRIC lease permits only observation/adoption of stable upstream configuration.
 * SDC3 owns host0x04023000, CCU+0xd30/+0xd3c, IRQ196 and DMA routes.
 * A board eMMC binding acquires SDC2|SDC3 together before touching either
 * selected host because both route to the same card. This grants no access
 * to the unselected host; shared pins and supply authority remain separate.
 * SDC1 owns host 0x04021000, CCU+0xd10/+0xd1c, IRQ194 and controller
 * DMA routes. SDC0 independently owns host 0x04020000 and CCU+0xd00/+0xd0c;
 * SDC2 owns host 0x04022000 and CCU+0xd20/+0xd2c. Each local scope includes
 * its controller, IRQ and DMA routes. All four are exclusive-only, with each
 * selected module parent reserved explicitly. Common storage gates/reset stay
 * under FABRIC; stable adoption never authorizes a shared storage reset.
 * PCIe local ownership grants no global SERDES or domain7 reset. A consumer
 * separately reserves COMB1 exclusively and SERDES_COMMON/USB_PCIE_DOMAIN
 * shared for stable adoption, or exclusive for changes. SERDES_COMMON covers
 * CCU+0x13c0/13c4 and SERDES AHB/MBUS gates; COMB1 covers its RTC reference
 * gate, local PHY, pipe mux and COMB1 reference controls. Global reset changes
 * require exclusion of COMB0/USB3 and all other affected users. These scopes
 * have no implicit parent: actual selected parents and FABRIC must be reserved
 * explicitly. Neither a token nor domain ownership proves source frequency,
 * PCK bus-clock ownership, rail stability or physical quiescence.
 * RADIO_MODULE
 * owns shared Wi-Fi/BT reset, firmware and power sequencing; it has no clock
 * ancestor and grants no pin or rail authority. Independent UART/SDIO operations
 * borrow that one lifetime owner rather than acquire separate module ownership.
 * S_TWI0 owns 0x07083000, all legacy/autonomous PMU/DRV engines and DMA,
 * R_CCU+0x19c gate0/reset16 and IRQ235; it reserves APBS1 -> REF -> OSC.
 * Every remote and local engine writer must be excluded before admission.
 * PL0/PL1 and electrical authority remain separate. FABRIC additionally covers
 * STORE_AHB/SD-monitor CCU+0x5c0 bits24/29, STORE_MBUS+0x5e0 bit30 and
 * MSI-LITE1+0x59c gate0/reset16/17. These shared storage fields cannot be
 * changed under an SDC1 token; exclusive FABRIC excludes all other consumers.
 * Changing a child holds its ancestors stable. Local controller scopes are
 * exclusive only; their selected parent must be leased separately. I2S/ASRC/
 * OWA/DMIC local ownership includes module clock, gate/reset and producer/FIFO
 * route writers. Pins, voltage, DMA channels and memory are separate leases.
 * FABRIC includes APB1 at CCU+0x518; every selector/divider/global-gate
 * writer requires exclusive FABRIC. Shared consumers separately validate and
 * reserve their selected parent. This software contract does not establish
 * remote-firmware exclusion or a stable live rate by itself.
 * FABRIC stability does not imply parent stability: callers must also lease
 * each selected PLL/reference parent. DMA0 requires a platform handoff covering
 * all channels and remote-MCU/routing writers; idle bits cannot establish it.
 * PLL_NPU retuning requires exclusion of every parent consumer, including
 * memory/fabric consumers. NPU local ownership alone cannot authorize it.
 * Media local scopes are exclusive. VI includes all camera controllers and
 * common analog words; DE_SYS includes both outputs and shared internal memory;
 * VO0 includes all shared TOP words and every output-0 consumer. IOMMU_SYS owns
 * both banks, but never establishes exclusion or drain of their ten masters.
 * Every overlapping writer, including remote firmware, must be bound to these
 * scopes. Whole-domain power changes additionally require stable PCK ownership
 * and verified quiescence. PLL retuning excludes every selected consumer.
 * Stable PCK clock and DCDC2 rail ownership remain separate platform leases.
 * DCDC2 shared/exclusive rail authority also uses this registry. Every PMIC
 * voltage/enable writer must acquire its exclusive scope; a mutex alone only
 * serializes transactions. DCDC2 has no clock ancestors or audio-profile effect.
 * Granting a token precedes all caller hardware operations. An uncertain
 * operation requires retain(), never release(). All refusal outputs unchanged.
 */
enum a733_clock_result a733_clock_acquire(struct a733_clock_leases *, uint64_t owner,
    a733_clock_mask_t shared, a733_clock_mask_t exclusive, uint64_t deadline, struct a733_clock_token *);
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
/* Atomic metadata publication after the privileged PLL coordinator has proved
 * its physical profile stable and kept all consumers excluded. parents must
 * be a nonempty subset of PLL_NPU|DISPLL1|USB01_REF held exclusively. Convert
 * only those bits to shared ownership, preserving owner and every other scope.
 * A fresh token invalidates the old identity. All validation/time callbacks
 * precede mutation; OK always returns the only live replacement token. Refusal
 * leaves both reservation and output unchanged (clock failure still poisons
 * the registry). Input/output and registry storage must be disjoint.
 * This does not prove a frequency, electrical state, consumer drain or boot
 * authority. The coordinator must replace all borrowed token copies before
 * admitting consumers and retain any uncertain handoff. Never release/reacquire
 * around this transition. The output has the same platform lifetime as input.
 */
enum a733_clock_result a733_clock_publish_stable(struct a733_clock_leases *,
    const struct a733_clock_token *, a733_clock_mask_t parents, uint64_t deadline,
    struct a733_clock_token *);
/* Read-only, nonblocking observation under the registry lock. OK requires an
 * exact live token in a healthy registry; BUSY admits no conclusion. No time
 * sample, state change, token release, or hardware access occurs. */
enum a733_clock_result a733_clock_token_status(struct a733_clock_leases *,const struct a733_clock_token *);
/* Retain all reservations and prohibit later acquisition/release/publication.
 * BUSY means the caller must retry this metadata operation before relinquishing
 * its existing exclusive ownership. No physical action or implicit reset. */
enum a733_clock_result a733_clock_retain(struct a733_clock_leases *, const struct a733_clock_token *);
#endif
