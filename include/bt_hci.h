#ifndef SUNSTONE_BT_HCI_H
#define SUNSTONE_BT_HCI_H
#include "bt_h4.h"

enum bt_hci_state { BT_HCI_CLOSED, BT_HCI_INITIALIZING, BT_HCI_IDENTIFIED, BT_HCI_FAULT, BT_HCI_CONFIGURING };
enum bt_hci_command_state { BT_HCI_IDLE, BT_HCI_SENDING, BT_HCI_WAITING };
struct bt_hci_identity {
    uint8_t version[8], supported_commands[64], features[8], buffer_size[7], address[6];
    unsigned valid; /* Six successful Reset/query results; not a connection state. */
};
struct bt_hci_incoming_config {
    uint16_t accept_timeout_slots, page_interval_slots, page_window_slots;
    uint64_t service_generation, receive_latency_us;
    /* Optional standard HCI CVSD audio: configure/read back synchronous
     * credits and erroneous data reporting before page scan or any ACL. Zero
     * preserves the eight-command Classic-only configuration. */
    unsigned synchronous_audio;
    /* Exact1 authenticates an actual scheduling/flow-control bound for this
     * firmware/UART lifetime. Includes wire/FIFO/staging backlog and maximum
     * owner-task service delay, including the last scan command/readback and
     * configured handoff. The bound ends at actual ACL request classification,
     * including COMPLETE/OWNED backlog and all intervening polls; it starts at
     * controller event emission. Bounded, non-reentrant,
     * no UART register access.
     * A chosen number alone is not evidence of that bound. The provider and
     * context stay live; every configured HCI step rechecks this authority. */
    int (*current)(void *,const struct bt_h4_binding *,uint64_t service_generation,
                   uint64_t receive_latency_us);
    void *context;
};
struct bt_hci {
    struct bt_h4 transport;
    const struct bt_hci *self;
    const void *acl_owner, *synchronous_owner, *security_owner;
    struct bt_hci_incoming_config incoming_config;
    uint64_t configuration_deadline;
    unsigned configuration_stage, configured, synchronous_ready, security_ack_pending;
    struct bt_hci_identity identity;
    uint64_t initialization_deadline, command_timeout_us, command_deadline, command_tag;
    uint16_t opcode;
    unsigned stage, credits, command_accepted, external_command;
    uint8_t last_status;
    enum bt_hci_command_state command;
    enum bt_hci_state state;
};
/* This is a serialized task worker, not a FreeRTOS task creator. One task
 * calls step after wakeup; it exclusively owns UART and all transport state.
 * Do not call H4 operations independently while the HCI worker owns it.
 * Initialization sends Reset, Read Local Version, Supported Commands, Local
 * Features, Buffer Size and BD_ADDR once each. No RF/link/profile commands.
 * Reset is authorized only after binding proves bootloader/patch completion
 * and a fresh H4/UART generation with no stale RX or earlier HCI owner.
 * Buffers, descriptors, binding and UART must be disjoint from this object.
 * Exact 1 accepted; -1 invalid/fault. No automatic retry or reset on failure. */
int bt_hci_init(struct bt_hci *, const struct bt_h4_binding *,
                const struct bt_h4_storage *, unsigned count,
                uint64_t initialization_deadline, uint64_t command_timeout_us);
/* Configure Classic incoming connections once, after the six bootstrap
 * commands and before any ACL claim or external command. Copies policy; no
 * UART access here. Requires no inherited RX, supported commands and BR/EDR.
 * Default step sends eight unique commands with exact Complete/readbacks:
 * event mask, clear filters, timeout write/read, page activity write/read,
 * page-only scan enable/read. Audio adds synchronous flow-control write/read
 * and erroneous-data-reporting write/read before scan, for twelve commands.
 * Scan starts last. No authentication-mode, key,
 * pairing, inquiry-discoverable or automatic acceptance command is issued.
 * Parameters use625us slots: timeout0x00a0..0xb540; even interval0x0012..0x1000;
 * window0x0011..interval. One absolute deadline and per-command bound apply.
 * Unsolicited/early incoming records stay owned through the subsequent ACL
 * handoff. On failure retain the entire generation; never retry or roll back. */
int bt_hci_configure_incoming(struct bt_hci *,const struct bt_hci_incoming_config *,
                              uint64_t deadline);
/* Check retained scheduling authority and the read-back timeout against a
 * proposed incoming response bound. Invalid policy returns0 without I/O;
 * changed authority faults HCI and returns-1. Does not establish pairing. */
int bt_hci_incoming_bound(struct bt_hci *,uint64_t response_timeout_us);
/* Read-only exact configured handoff identity, including optional audio stages. */
int bt_hci_configuration_complete(const struct bt_hci *);
/* Standard audio capability admission: Core4.1..5.4, Enhanced Setup/Accept,
 * Read Remote Features, synchronous buffers and required credit/error-report
 * commands. Returns1 supported,0 otherwise; no I/O or mutation. */
int bt_hci_synchronous_supported(const struct bt_hci *);
/* Post-identification leaf commands: Create Connection (0x0405,13 bytes) and
 * Disconnect (0x0406,3 bytes), Accept Connection Request (0x0409,7 bytes),
 * Reject Connection Request (0x040a,7 bytes). A registered synchronous owner
 * also uses Read Remote Features0x041b/2, Enhanced Setup0x043d/59,
 * Enhanced Accept0x043e/63 and Reject Synchronous0x042a/7 after audio config.
 * Exact1 accepted,0 busy/no credit,
 * -1 refused/fault. Accept and Reject require prior matching Request.
 * The exclusive link owner validates semantics and owns final link events.
 * Acknowledgement records remain OWNED; matching tag is command_tag after
 * acceptance. Successful Status only returns the command slot, never proves a
 * connection/disconnection. Caller supplies the entire absolute operation bound. */
int bt_hci_request(struct bt_hci *, uint16_t opcode, const uint8_t *, size_t,
                   uint64_t deadline);
/* Only the exact registered synchronous owner may issue these audio/link
 * commands; preserves the same Status-versus-terminal-event contract. */
int bt_hci_synchronous_request(struct bt_hci *,const void *owner,uint16_t opcode,
                              const uint8_t *,size_t,uint64_t deadline);
/* One registered BR/EDR security owner shares the existing command credit.
 * The exact opcode/length allowlist covers SSP/key/encryption setup only.
 * Complete commands retain their matching completion; authentication and
 * encryption commands retain Status and require separate terminal events.
 * Registration is permanent for this controller lifetime. */
int bt_hci_security_register(struct bt_hci *,const void *owner);
int bt_hci_security_acknowledge(struct bt_hci *,const void *owner,uint64_t tag);
int bt_hci_security_request(struct bt_hci *,const void *owner,uint16_t opcode,
                           const uint8_t *,size_t,uint64_t deadline);
/* One H4 step and at most one complete record dispatch. Commands consume one
 * credit at submission. Bootstrap Command Status marks acceptance only;
 * bootstrap advances after matching Command Complete. External link commands
 * retain Status/refusal records for the link owner and its terminal event.
 * Tagging records at UART acquisition prevents already-acquired bytes from
 * satisfying a later command. H4 drains preexisting FIFO/staging bytes before
 * transmission; it cannot timestamp bytes arriving between observations. No opcode is reused within this initialization lifetime. */
int bt_hci_step(struct bt_hci *, unsigned budget, uint64_t deadline);
/* Exposes earliest retained unsolicited event or ACL/SCO/ISO packet. Data bytes
 * and raw flags are unchanged; no ACL reassembly, audio decoding, link/security
 * state, or ISO feature support is implied. A successful Disconnect Complete
 * remains a record for the link owner to process. Handle reuse must be
 * resolved by that owner; this worker never treats it as a query completion.
 * After FAULT records remain inspectable, but release is refused. */
const struct bt_h4_record *bt_hci_next(const struct bt_hci *);
int bt_hci_release(struct bt_hci *, uint64_t serial);
#endif
