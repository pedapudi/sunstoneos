/* Export the selected native declarations for a modeled external wire input.
 * The Python check executes the image's actual identity and validators. */
#include "../apps/os/os.h"
#include "../apps/bluetooth/bluetooth.h"
#include <stddef.h>
struct wire_field { char name[56]; uint64_t value; };
_Static_assert(sizeof(struct wire_field)==64,"Wire layout fixture record");
#define P(member) {"profile." #member,offsetof(struct bluetooth_profile,member)}
const struct wire_field os_headset_wire_layout[]={
    {"profile.size",sizeof(struct bluetooth_profile)},
    {"profile.magic_value",BLUETOOTH_PROFILE_MAGIC},
    {"profile.address",OS_HEADSET_PROFILE_ADDRESS},
    {"publication.address",BLUETOOTH_PROFILE_ADDRESS},
    P(magic),P(version),P(bytes),P(topology),P(sid_authority),P(radio_authority),
    P(uart_authority),P(wake_authority),P(wiring_evidence),P(sid_evidence),
    P(fresh_transport_evidence),P(pm_latch),P(radio),P(radio.fabric),P(radio.fabric_hz),
    P(radio.expected),P(radio.desired),P(wake.authority),P(uart.owner),P(uart.route),
    P(uart.format),P(uart.deferred_irq),P(identity),P(firmware_address),P(firmware_bytes),
    P(session_owner),P(runtime_lease_us),P(renew_margin_us),P(command_timeout_us),
    P(frame_timeout_us),P(binding_timeout_us),P(headset),P(headset.minimum_security),
    P(headset.scheduling_authority),P(headset.scheduling_evidence),P(headset.receive_latency_us),
    P(headset.response_timeout_us),P(headset.samples),P(recognition),P(bluetooth_stop),
    P(bluetooth_stop.wiring_evidence),P(bluetooth_stop.expected),
};
