#ifndef SUNSTONE_BLUETOOTH_APPLICATION_H
#define SUNSTONE_BLUETOOTH_APPLICATION_H
#include "a733_radio_session.h"
#include "a733_range.h"
#include "a733_radio_platform_native.h"
#include "a733_uart_platform_native.h"
#include "a733_native_usb.h"
#include "bt_hci.h"
#include "recognition.h"
#include "../board/identity.h"
#define BLUETOOTH_PROFILE_MAGIC UINT64_C(0x3150465442535353)
#define BLUETOOTH_PROFILE_ADDRESS UINT64_C(0x43000000)
/* Immutable external authority; evidence identifies the actual installed
 * topology, fresh module/UART ownership and secure SID access. PM2 is never
 * driven here: stable reset/LPO behavior must already be established. */
struct bluetooth_headset_profile {
    uint32_t mode,incoming;
    uint8_t peer[6],pin[17],allow_just_works;
    uint32_t minimum_security,server_channel;
    uint64_t scheduling_authority;
    uint8_t scheduling_evidence[32];
    uint64_t receive_latency_us,response_timeout_us;
    uint32_t samples;
    uint16_t mtu,accept_slots,page_interval,page_window;
    uint32_t line_timeout_us,capture_timeout_us;
    uint8_t peer_page_scan_mode,peer_role_switch;
    uint16_t peer_clock_offset;
    uint64_t reserved2;
};
_Static_assert(sizeof(struct bluetooth_headset_profile)==128,"Headset immutable profile geometry");
struct bluetooth_profile {
    uint64_t magic;uint32_t version,bytes;
    uint64_t topology,sid_authority,radio_authority,uart_authority,wake_authority;
    uint8_t wiring_evidence[32],sid_evidence[32],fresh_transport_evidence[32];
    struct a733_pin_latch_handoff pm_latch;
    struct a733_radio_platform_config radio;
    struct a733_radio_bluetooth_pins wake;
    struct a733_uart_platform_config uart;
    struct aic8800_transport_config transport;
    struct aic8800_identity_admission bootstrap;
    struct aic8800_radio_identity identity;
    uint64_t firmware_address[4],firmware_bytes[4];
    uint64_t session_owner,runtime_lease_us,renew_margin_us;
    uint64_t command_timeout_us,frame_timeout_us,binding_timeout_us;
    struct bluetooth_headset_profile headset;
    struct bluetooth_recognition_profile recognition;
};
_Static_assert(sizeof(struct bluetooth_profile)==832&&offsetof(struct bluetooth_profile,recognition)==752&&offsetof(struct bluetooth_profile,headset)==624&&offsetof(struct bluetooth_profile,firmware_address)==512,
    "Bluetooth offline profile geometry");
/* Session lifetimes: supervised, exactly 60 s after the session start, or
 * resident, ending at UINT64_MAX/24 us like the board's resident session. */
#define BLUETOOTH_SUPERVISED_US UINT64_C(60000000)
#define BLUETOOTH_RESIDENT_END_US (UINT64_MAX/24)
/* Resident firmware bootstrap bound after the session start, unchanged when
 * requesting four-bit 25 MHz. It matches the integrated OS radio's bound;
 * this is configuration, not a measurement. */
#define BLUETOOTH_RESIDENT_BOOTSTRAP_US UINT64_C(120000000)
/* One actual owner task. Providers, profiles, identity and all buffers remain
 * disjoint and retained through terminal failure and manual power removal.
 *
 * The module (power, SDIO card, firmware and runtime) is the application's
 * own radio platform and bootstrap session, or, after
 * bluetooth_application_attach, a full Wi-Fi session that another owner built
 * on the same task. module_radio and module_session name the one in use.
 *
 * started is the origin of the stage schedule, which keeps the offsets of the
 * supervised 60 s session. In the resident lifetime the origin moves when the
 * firmware first becomes ready, to the point where the supervised bootstrap
 * bound would fall, so each later stage keeps its supervised time budget. */
struct bluetooth_application {
    struct bluetooth_application *self;
    const struct bluetooth_profile *profile;
    struct a733_native_usb *shared;
    const struct board_radio_chip_id *chip_id;
    TaskHandle_t owner;
    atomic_uint *recovery_state;
    struct a733_radio_platform_native radio_native;
    struct a733_uart_platform_native uart_native;
    struct a733_radio_platform radio;
    struct a733_uart_platform uart;
    struct a733_radio_session session;
    struct a733_radio_session_config session_config;
    struct a733_uart_platform_config uart_config;
    struct a733_radio_platform_config radio_config;
    struct bt_hci hci;
    struct bt_h4_binding binding;
    struct bt_h4_storage slots[4];
    uint8_t packets[4][1024];
    struct aic8800_dispatch_event event;
    struct aic8800_dispatch_record diagnostic;
    struct a733_radio_platform *module_radio;
    struct a733_radio_session *module_session;
    uint64_t firmware_generation,uart_generation,deadline,hci_deadline,bootstrap_deadline;
    unsigned stage,iteration_live,host_wake,attached,resident;
    uint64_t started;
    atomic_uint failed,result_lock,result;
    struct bt_hci_identity identity;
    uint64_t discarded_records;
};
/* Next slot of a fixed-period synchronous TX stream. It advances from the
 * previous slot rather than from the 1 ms poll that served it, so poll rounding
 * cannot stretch the period below the eSCO consumption rate. Up to two periods
 * of lag are caught up back to back, within the SCO packet pool; a longer stall
 * restarts the schedule at now once and counts it. The result is capped. */
static inline uint64_t bluetooth_cadence(uint64_t slot,uint64_t now,uint64_t period,uint64_t limit,uint64_t *restarts)
{
    uint64_t next=a733_deadline_after_saturating(slot,period);
    if(now>next&&now-next>=2*period){next=now;++*restarts;}
    return next<limit?next:limit;
}
/* End of repeated capture: 38 s after the schedule origin when supervised;
 * none when resident. */
static inline uint64_t bluetooth_capture_window(const struct bluetooth_application *a)
{return a->resident?UINT64_MAX:a->started+38000000;}
/* Bound for closing the stream once capture stops: 40 s after the origin when
 * supervised; 2 s after the stop when resident. */
static inline uint64_t bluetooth_close_deadline(const struct bluetooth_application *a,uint64_t now)
{return a->resident?a733_deadline_after_saturating(now,2000000):a->started+40000000;}
int bluetooth_profile_valid(const struct bluetooth_profile *,const uint8_t[32]);
/* Before scheduling. end_us - start_us is 60 s, or end_us is
 * BLUETOOTH_RESIDENT_END_US. The application builds and owns the module. */
int bluetooth_application_init(struct bluetooth_application *,const struct bluetooth_profile *,
    struct a733_native_usb *,const struct board_radio_chip_id *,TaskHandle_t,atomic_uint *,uint64_t start_us,uint64_t end_us,
    const uint8_t profile_digest[32]);
/* Before scheduling, with the lifetime rules of init: attach to a module
 * owned by a full Wi-Fi session that no task has polled yet. The profile must
 * name that module: its platform and session owners, topology, radio identity
 * and firmware spans. The Bluetooth wake pins join the module platform, and
 * its native adapter authenticates them in an added admission. The Bluetooth
 * owner must be the session's owner task, which polls the application after
 * the session. The application owns only UART1, H4/HCI and its upper layers;
 * latch is the client's own recovery latch. A Bluetooth failure retains those
 * and leaves the module and Wi-Fi running; a module fault ends Bluetooth. */
int bluetooth_application_attach(struct bluetooth_application *,const struct bluetooth_profile *,
    struct a733_native_usb *,const struct board_radio_chip_id *,TaskHandle_t,atomic_uint *latch,uint64_t start_us,uint64_t end_us,
    const uint8_t profile_digest[32],struct a733_radio_platform_native *,struct a733_radio_session *);
/* 0 pending,1 actual six-command identity captured,-1 retained failure. */
int bluetooth_application_poll(struct bluetooth_application *);
int bluetooth_application_retain(struct bluetooth_application *);
int bluetooth_application_result(struct bluetooth_application *,struct bt_hci_identity *);
#endif
