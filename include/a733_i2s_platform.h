#ifndef SUNSTONE_A733_I2S_PLATFORM_H
#define SUNSTONE_A733_I2S_PLATFORM_H
#include "a733_i2s.h"
#include "a733_pins.h"
#include "a733_i2s_stream.h"
struct a733_audio_leases;

/* Clock/resource authority is supplied by the platform's common lease service.
 * It must coordinate EVERY writer, including GPU users of the reference PLL.
 * These scopes are capabilities with the following mandatory coverage:
 * REFERENCE: shared stable oscillator selection and PLL_REF configuration.
 * AUDIO_STABLE: shared AUDIO1 N, DIV5, SDM, gates and auto-gating stability;
 *               DIV2 users must also prevent any change to the shared N.
 * AUDIO_CHANGE: exclusive AUDIO1, BOTH outputs and I2S0/1/2/2-ASRC/3/4,
 *               OWA-TX and DMIC clocks, including their reset/gate writers.
 * LOCAL: exclusive I2S0 module, bus/reset, controller and FIFO DMA route.
 * The register mutex alone never establishes these capabilities. */
enum a733_audio_scope {
    A733_AUDIO_REFERENCE = 1, A733_AUDIO_STABLE, A733_AUDIO_CHANGE, A733_AUDIO_LOCAL
};
struct a733_audio_token { uint64_t id; enum a733_audio_scope scope; };
struct a733_audio_operations {
    /* Acquire may be called again with its existing token after PENDING.
     * READY/PENDING/RETAINED populate or preserve a nonzero token. BUSY never
     * changes tokens or hardware. Unknown outcomes are retained failures. */
    enum a733_i2s_resource_result (*acquire)(void *, enum a733_audio_scope,
        struct a733_audio_token *, uint64_t deadline_us);
    /* Atomic conversion from AUDIO_CHANGE to shared AUDIO_STABLE, publishing
     * N=128, DIV5=5 and modulation/auto-gating off before allowing consumers.
     * Old token is immutable; new token is populated on READY/PENDING/RETAINED.
     * Repeated PENDING calls use the same old/new identities. */
    enum a733_i2s_resource_result (*publish)(void *, const struct a733_audio_token *,
        struct a733_audio_token *, uint64_t deadline_us);
    /* Only LOCAL is released here. The token remains unchanged on all results;
     * caller clears its record only after an on-time READY. BUSY has no effects;
     * PENDING retains ownership, RETAINED prohibits further operations. */
    enum a733_i2s_resource_result (*release)(void *, const struct a733_audio_token *, uint64_t);
    int (*enter)(void *, uint64_t deadline_us);
    void (*leave)(void *);
    int (*now_us)(void *, uint64_t *);
    int (*read)(void *, uintptr_t, uint32_t *);
    int (*write)(void *, uintptr_t, uint32_t);
};
/* Platform-lifetime storage, serialized with every local binding using it.
 * Parent leases survive local close. No shutdown/retune API is provided.
 * Failure preserves every identity and prohibits further operations. */
struct a733_audio_parent {
    struct a733_audio_operations ops;
    void *cookie;
    struct a733_audio_token reference, audio, published;
    uint64_t deadline, last_time, wait_since;
    uint32_t audio_value;
    unsigned stage, cold, locked_samples, failed, initialized;
};
/* No MMIO. cold=1 authorizes a cold start ONLY after AUDIO_CHANGE is admitted;
 * cold=0 only adopts the exact active profile. This flag conveys no lease. */
int a733_audio_parent_init(struct a733_audio_parent *, const struct a733_audio_operations *,
    void *, unsigned cold, uint64_t deadline_us);
enum a733_i2s_resource_result a733_audio_parent_poll(struct a733_audio_parent *);
int a733_audio_parent_ready(const struct a733_audio_parent *);

struct a733_i2s_platform {
    struct a733_audio_parent *parent;
    struct a733_pins *pins;
    struct a733_pin_request requests[5];
    size_t count;
    uint64_t owner, deadline, wait_since;
    struct a733_audio_token local;
    struct a733_pin_lease pin_lease;
    struct a733_i2s_plan plan;
    uint32_t initial_module, initial_bus;
    unsigned stage, failed, initialized;
    struct a733_audio_leases *stream_leases;
    uint64_t stream_deadline;
    uint32_t route_mask;
    unsigned stream_mode, register_retained;
};
/* No MMIO. Supply expected inherited pin fields and approved electrical fields;
 * selected pins are PB5/6/8, optional PB4, desired function 3. Bank voltage is
 * unchanged. Pins and clock resources must share the same register mutex,
 * but no lease/pin callback is ever invoked while that mutex is held here.
 * DMA0 clock/memory/channel service is a separate prerequisite. */
int a733_i2s_platform_init(struct a733_i2s_platform *, struct a733_audio_parent *,
    struct a733_pins *, uint64_t owner, const struct a733_pin_request *, size_t);
/* Pass binding as cookie to the capture core. It supports 16 kHz, two 32-bit
 * slots, 24.576 MHz module, optional 12.288 MHz MCLK. */
struct a733_i2s_operations a733_i2s_platform_operations(void);
/* Stream-only binding to actual common audio leases. PB7 function 3 is DOUT0;
 * PB8 function 3 is DIN0. PB5/6 and optional PB4 remain shared clock pins.
 * Bank voltage and external microphone/codec readiness are separate authority.
 * parent must use leases.operations with leases as cookie. The parent backend
 * and pin manager MUST use the same platform register mutex and monotonic
 * provider/history; this initializer cannot discover those physical facts.
 * Calls remain serialized with parent/leases/pins for platform lifetime.
 * Stream register wrappers enter that mutex for each access. Failed/late I/O
 * preserves its held mutex and latches register_retained; no further callbacks.
 * Expiry first seen after a clean unlock retains tokens, not mutex ownership.
 * New public fields enlarge private storage: rebuild every embedding consumer.
 */
int a733_i2s_platform_stream_init(struct a733_i2s_platform *,struct a733_audio_parent *,
    struct a733_pins *,struct a733_audio_leases *,uint64_t owner,
    const struct a733_pin_request *,size_t,enum a733_i2s_direction);
int a733_i2s_platform_duplex_init(struct a733_i2s_platform *,struct a733_audio_parent *,
    struct a733_pins *,struct a733_audio_leases *,uint64_t,
    const struct a733_pin_request *,size_t);
struct a733_i2s_stream_operations a733_i2s_platform_stream_operations(void);
#endif
