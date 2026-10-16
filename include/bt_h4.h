#ifndef SUNSTONE_BT_H4_H
#define SUNSTONE_BT_H4_H
#include "a733_uart.h"

#define BT_H4_SLOTS 8u
#define BT_H4_BUDGET 64u
#define BT_H4_MAX_PACKET 65539u
/* Packet bytes include the HCI header, excluding the H4 type byte. Event,
 * ACL, SCO and ISO are retained unchanged; payload semantics remain external. */
struct bt_h4_storage { uint8_t *bytes; size_t capacity; };
enum bt_h4_owner { BT_H4_FREE, BT_H4_PARTIAL, BT_H4_COMPLETE, BT_H4_OWNED };
struct bt_h4_record {
    struct bt_h4_storage storage;
    size_t length, expected;
    uint64_t serial, tag;
    unsigned type, header;
    enum bt_h4_owner owner;
};
struct bt_h4_binding {
    struct a733_uart *uart;
    uint64_t firmware_generation, uart_generation, frame_timeout_us;
    uint32_t baud;
    /* Exact 1 attests live firmware bootstrap completion, H4 mode/baud,
     * exclusive UART1/pin/rail/clock ownership, and unchanged generations.
     * Bounded, non-reentrant; it must not access UART data/status registers. */
    int (*current)(void *, uint64_t firmware_generation, uint64_t uart_generation);
    void *context;
};
enum bt_h4_state { BT_H4_CLOSED, BT_H4_ACTIVE, BT_H4_FAULT };
/* Zero-init once. One serialized task owns transport and UART for the entire
 * lifetime. Other task/ISR UART accesses are forbidden. Keep the UART GIC
 * interrupt masked; the platform schedules bounded task polls. No buffer/object reinitialization on fault.
 * Storage and object lifetimes extend through external recovery on any fault.
 * No implicit firmware load, reset, generation change or H4 resynchronization. */
struct bt_h4 {
    struct bt_h4_binding binding;
    struct a733_uart_plan uart_plan;
    struct bt_h4_record records[BT_H4_SLOTS];
    struct a733_uart_byte staging[BT_H4_BUDGET];
    uint8_t tx[259];
    size_t tx_length, tx_offset, staged, stage_offset;
    uint64_t now, deadline, partial_deadline, tx_deadline, next_serial, stage_tag;
    unsigned count, assembling;
    enum bt_h4_state state;
};
/* All slots must be disjoint from each other, UART, transport and config;
 * capacity 257..65539. Invalid input leaves object untouched and does no I/O.
 * A full pool retains staged bytes and stops FIFO reads; peer RTS/CTS and
 * worker scheduling must prevent overrun. A timeout still faults this lifetime. */
int bt_h4_init(struct bt_h4 *, const struct bt_h4_binding *,
               const struct bt_h4_storage *, unsigned count, uint64_t deadline);
/* Commands only; parameters copied before return, length <=255. No automatic
 * retransmission. Returns 1 accepted, 0 busy, -1 invalid/fault. */
int bt_h4_command(struct bt_h4 *, uint16_t opcode, const uint8_t *, size_t,
                  uint64_t deadline);
/* At most budget RX bytes, budget parser bytes and budget TX bytes. RX uses
 * one exchange; TX uses a second only after an RX-empty observation with no
 * staged, partial or worker-unprocessed complete records. OWNED records do not
 * block TX while a free slot permits that observation. tag labels eligibility
 * at RX acquisition, not physical arrival time. It remains attached through
 * staging. Fault retains all cursors and records. */
int bt_h4_step(struct bt_h4 *, unsigned budget, uint64_t tag, uint64_t deadline);
/* Earliest record with selected ownership. Pointer is immutable until exact
 * serial release; no ownership mutation by caller. COMPLETE is worker-private,
 * OWNED is application-visible. On fault records remain inspectable. */
const struct bt_h4_record *bt_h4_next(const struct bt_h4 *, enum bt_h4_owner);
int bt_h4_own(struct bt_h4 *, uint64_t serial);
int bt_h4_release(struct bt_h4 *, uint64_t serial);
void bt_h4_fail(struct bt_h4 *);
#endif
