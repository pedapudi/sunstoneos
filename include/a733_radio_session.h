#ifndef SUNSTONE_A733_RADIO_SESSION_H
#define SUNSTONE_A733_RADIO_SESSION_H
#include "a733_radio_platform.h"
#include "aic8800_firmware.h"
#include "aic8800_runtime.h"
#include "aic8800_supplicant.h"
#include "sunstone_net.h"
struct a733_radio_session_config {
    TaskHandle_t owner_task;
    uint64_t owner,bootstrap_deadline,runtime_lease_us,renew_margin_us;
    struct aic8800_transport_config transport;
    struct aic8800_identity_admission bootstrap;
    struct aic8800_firmware_config firmware;
    struct aic8800_runtime_config runtime;
    struct aic8800_wifi_config wifi;
};
enum a733_radio_session_result { A733_SESSION_PENDING,A733_SESSION_BUSY,
    A733_SESSION_MANAGEMENT,A733_SESSION_NETWORK,A733_SESSION_RETAINED,A733_SESSION_INVALID };
/* Private, zero storage. One immutable task owns every platform/protocol/socket
 * polling call. Application tasks use the existing copied socket queues. All
 * object and blob lifetimes survive fault; no shutdown, reset or reinit exists. */
struct a733_radio_session {
    struct a733_radio_session *self;
    struct a733_radio_session_config config;
    struct a733_radio_platform *platform;
    struct a733_radio_borrow borrow;
    struct aic8800_sdio_transport transport;
    struct aic8800_firmware firmware;
    struct aic8800_runtime runtime;
    struct aic8800_wifi wifi;
    struct aic8800_data data;
    struct aic8800_security security;
    struct aic8800_supplicant supplicant;
    struct aic8800_dispatch dispatcher;
    struct sunstone_net sockets;
    atomic_uint busy;
    uint64_t deadline,last_now;
    unsigned stage,fault,renew_pending;
    unsigned bootstrap_only,reconnect_phase;
    uint64_t reconnect_deadline,reconnect_open;
    uint16_t reconnect_reason;
    struct aic8800_wifi_target reconnect_target;
    struct aic8800_supplicant_config reconnect_security;
};
/* Pure init, before first platform poll. Requires the actual initialized
 * platform and immutable firmware blobs. Firmware identity is an expectation;
 * the actual transport reads it again before the engine can claim ownership.
 * All callbacks are bounded and non-reentrant; callback backing is separate
 * from the retained object graph. Constructors consume fresh storage even if
 * a later profile is refused; no partial initialization may be retried.
 * Runtime chip-ID and entropy callbacks remain real platform prerequisites;
 * no identifier, timer or deterministic entropy fallback is supplied. */
int a733_radio_session_init(struct a733_radio_session *,struct a733_radio_platform *,
    const struct a733_radio_session_config *);
/* Same actual firmware/runtime/dispatcher graph, without Wi-Fi management,
 * security or IP activation. No entropy or network credentials are required. */
int a733_radio_session_init_bootstrap(struct a733_radio_session *,struct a733_radio_platform *,
    const struct a733_radio_session_config *);
/* Owner-only authenticated completed firmware generation; 1 ready,0 pending/
 * contention,-1 invalid/fault. Readiness establishes bootstrap, not HCI. */
int a733_radio_session_bootstrap(struct a733_radio_session *,uint64_t *generation);
enum a733_radio_session_result a733_radio_session_poll(struct a733_radio_session *);
/* Owner-only external prerequisite failure. Closes network admission before
 * further protocol work and retains the existing complete resource graph. */
enum a733_radio_session_result a733_radio_session_retain(struct a733_radio_session *);
/* Serialized owner task only. Each admission directly authenticates the real
 * platform borrow. Deadlines must fit the current runtime lease. RF, association
 * and open-port authority remain explicit lower-layer inputs. */
/* Healthy, same-AP reconnect with fixed IPv4 or DHCP. All sockets must have completed
 * CLOSE and every copied reply must be consumed. New submissions stay closed
 * until the same IP interface publishes a fresh address. Fault/unsolicited loss
 * is refused. DHCP restarts in the IP task; security config is required for WPA2.
 * Success admits bounded work; poll returns PENDING until NETWORK is restored. */
int a733_radio_session_reconnect(struct a733_radio_session *,const struct aic8800_wifi_target *,
    const struct aic8800_supplicant_config *,uint64_t open_authority,uint16_t reason,uint64_t deadline);
int a733_radio_session_scan(struct a733_radio_session *,uint64_t deadline);
int a733_radio_session_connect(struct a733_radio_session *,const struct aic8800_wifi_target *,uint64_t deadline);
int a733_radio_session_disconnect(struct a733_radio_session *,uint16_t reason,uint64_t deadline);
int a733_radio_session_authorize_open(struct a733_radio_session *,uint64_t record);
int a733_radio_session_start_security(struct a733_radio_session *,const struct aic8800_supplicant_config *,uint64_t deadline);
/* Config must name this session's transport, dispatcher and recorded owner task.
 * Real entropy providers and completed data/security authorization are required.
 * Actual FreeRTOS+TCP and bounded socket queues stay on the same polling task. */
int a733_radio_session_start_network(struct a733_radio_session *,const struct sunstone_net_config *);
/* Copies are independent of the entire retained session/platform graph and blobs.
 * Event/diagnostic copies remain available after fault; their bytes grant no RF
 * or network authority. Scan completion requires a healthy runtime. */
int a733_radio_session_take_event(struct a733_radio_session *,struct aic8800_dispatch_event *);
int a733_radio_session_take_record(struct a733_radio_session *,struct aic8800_dispatch_record *);
int a733_radio_session_take_scan(struct a733_radio_session *,struct aic8800_wifi_scan_summary *);
#endif
