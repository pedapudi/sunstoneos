#ifndef SUNSTONE_BLUETOOTH_APPLICATION_H
#define SUNSTONE_BLUETOOTH_APPLICATION_H
#include "a733_radio_session.h"
#include "a733_radio_platform_native.h"
#include "a733_uart_platform_native.h"
#include "a733_native_usb.h"
#include "bt_hci.h"
#include "../board/identity.h"
#define BLUETOOTH_PROFILE_MAGIC UINT64_C(0x3150465442535353)
#define BLUETOOTH_PROFILE_ADDRESS UINT64_C(0x43000000)
/* Immutable external authority; evidence identifies the actual installed
 * topology, fresh module/UART ownership and secure SID access. PM2 is never
 * driven here: stable reset/LPO behavior must already be established. */
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
};
_Static_assert(sizeof(struct bluetooth_profile)==624&&offsetof(struct bluetooth_profile,firmware_address)==512,
    "Bluetooth offline profile geometry");
/* One actual owner task. Providers, profiles, identity and all buffers remain
 * disjoint and retained through terminal failure and manual power removal. */
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
    uint64_t firmware_generation,uart_generation,deadline,hci_deadline;
    unsigned stage,iteration_live,host_wake;
    atomic_uint failed,result_lock,result;
    struct bt_hci_identity identity;
    uint64_t discarded_records;
};
int bluetooth_profile_valid(const struct bluetooth_profile *,const uint8_t[32]);
int bluetooth_application_init(struct bluetooth_application *,const struct bluetooth_profile *,
    struct a733_native_usb *,const struct board_radio_chip_id *,TaskHandle_t,atomic_uint *,uint64_t start_us,uint64_t end_us,
    const uint8_t profile_digest[32]);
/* 0 pending,1 actual six-command identity captured,-1 retained failure. */
int bluetooth_application_poll(struct bluetooth_application *);
int bluetooth_application_retain(struct bluetooth_application *);
int bluetooth_application_result(struct bluetooth_application *,struct bt_hci_identity *);
#endif
