#ifndef SUNSTONE_A733_DSI_H
#define SUNSTONE_A733_DSI_H
#include <stddef.h>
#include <stdint.h>

#define A733_DSI_COMMAND_BYTES 256u
#define A733_DSI_LONG_PAYLOAD_MAX 250u
struct a733_dsi_packet {
    uint8_t bytes[A733_DSI_COMMAND_BYTES];
    uint16_t size;
};
/* DCS short writes 0x05/0x15 require one/two bytes. Generic short writes
 * 0x03/0x13/0x23 require zero/one/two bytes. Long writes 0x29/0x39 admit
 * 1..250 payload bytes: four header bytes and two CRC bytes also occupy SRAM.
 * Virtual channels 0..3. Output is unchanged on refusal; output and payload
 * storage must not overlap. This constructs a packet without accessing hardware.
 */
int a733_dsi_encode(struct a733_dsi_packet *, unsigned type, unsigned channel,
                    const void *payload, size_t size);

enum a733_dsi_state { A733_DSI_IDLE, A733_DSI_PENDING,
    A733_DSI_ENGINE_DONE, A733_DSI_RETAINED };
enum a733_dsi_error { A733_DSI_OK, A733_DSI_TIME, A733_DSI_IO,
    A733_DSI_AUTHORITY, A733_DSI_INITIAL_STATE, A733_DSI_VERIFY,
    A733_DSI_REENTRY };
struct a733_dsi_ops {
    int (*admit)(void *, unsigned controller, unsigned lanes, uint64_t lease);
    int (*read32)(void *, uintptr_t, uint32_t *);
    int (*write32)(void *, uintptr_t, uint32_t);
    int (*barrier)(void *);
    int (*now_us)(void *, uint64_t *);
};
struct a733_dsi_config {
    struct a733_dsi_ops ops;
    void *context;
    uint64_t lease;
    unsigned controller, lanes;
};
struct a733_dsi {
    struct a733_dsi_config config;
    struct a733_dsi_packet packet;
    uint64_t deadline, last_now;
    uint32_t control;
    enum a733_dsi_state state;
    enum a733_dsi_error error;
    unsigned initialized, entered, step, position, writes_attempted;
};
/* Privileged serialized owner, fresh zero storage, no reinitialization.
 * Configuration is copied; callback/context storage remains private, disjoint
 * from the controller and all submitted buffers, and live for its lifetime.
 * All callbacks return exactly 1 on success and are bounded/non-reentrant.
 * read32/write32 access ordered Device memory; barrier completes prior writes.
 * admit must authenticate an existing combined host/PHY lifetime lease and
 * prove stable power, pins, clocks, released shared reset, configured PHY,
 * low-power lane readiness and exclusive command ownership. Other firmware,
 * video producers and interrupt handlers must be quiescent. It must not infer
 * those conditions from a nonzero lease alone. The platform provider is not
 * implemented here. Independently qualified watchdog recovery is required.
 */
int a733_dsi_init(struct a733_dsi *, const struct a733_dsi_config *);
/* Copies the payload and encoded packet before returning. Deadline is an
 * absolute monotonic microsecond time. One outstanding packet; only IDLE admits
 * submission. Refusal leaves the controller unchanged and invokes no callback.
 */
int a733_dsi_submit(struct a733_dsi *, unsigned type, unsigned channel,
                    const void *payload, size_t size, uint64_t deadline);
/* One register/barrier/admission operation per poll, with pre/post time checks.
 * ENGINE_DONE means the local instruction engine stopped after the start write.
 * It does not establish panel reception or acceptance. No ACK, BTA, readback,
 * video mode, clock programming, reset, rollback or automatic retry occurs.
 * Every fault retains the combined lease and packet; terminal polls are quiet.
 */
enum a733_dsi_state a733_dsi_poll(struct a733_dsi *);
/* Acknowledge ENGINE_DONE and permit another packet. No resource release. */
int a733_dsi_reap(struct a733_dsi *);
#endif
