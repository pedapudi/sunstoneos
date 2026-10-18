#ifndef SUNSTONE_BOARD_RADIO_H
#define SUNSTONE_BOARD_RADIO_H
#include "a733_native_usb.h"
#include "a733_radio_entropy.h"
#include "a733_radio_platform_native.h"
#include "a733_radio_session.h"
#include "identity.h"
struct board_radio_config {
    struct a733_native_usb *shared;
    struct a733_radio_platform_config platform;
    struct a733_radio_session_config session;
    struct a733_radio_entropy_config entropy;
    struct aic8800_wifi_target target;
    struct aic8800_supplicant_config security;
    struct sunstone_net_config network;
    const struct board_radio_chip_id *chip_id;
    int (*admit)(void *,const struct a733_radio_platform_config *,const struct a733_clock_token *);
    void *admission_context;
    atomic_uint *recovery_state; /* Same latch as actual hardware reload CAS. */
    uint64_t startup_deadline_us,association_timeout_us,security_timeout_us;
};
enum board_radio_result { BOARD_RADIO_PENDING,BOARD_RADIO_NETWORK,
    BOARD_RADIO_RETAINED,BOARD_RADIO_INVALID };
struct board_radio {
    struct board_radio *self;
    struct board_radio_config config;
    struct a733_radio_platform_native native;
    struct a733_radio_platform platform;
    struct a733_radio_session session;
    struct a733_radio_entropy entropy;
    struct a733_ce_entropy ce_source;
    atomic_uint failed,network_started;
    atomic_uint_fast64_t completed_transfers;
    atomic_uint_fast64_t watchdog_deadline;
    atomic_uint snapshot_busy;
    atomic_uint client_status,verified_received;
    const struct board_radio_profile *startup_profile;
    uint64_t client_ticket,client_handle;
    size_t client_sent,client_received;
    unsigned client_stage,client_pending;
    struct sunstone_net_request client_request;
    struct sunstone_net_reply client_reply;
    /* Owner-task diagnostic copies. Startup events drain before socket clients
     * exist; only the latest copy is kept, with explicit consumed counts. */
    struct aic8800_dispatch_event startup_event;
    struct aic8800_dispatch_record diagnostic;
    uint64_t startup_events,diagnostic_records;
    unsigned stage,constructed;
    uint64_t operation_deadline;
};
/* Pointer-free immutable boot profile, authenticated by the board session.
 * Evidence describes actual external qualification; no field qualifies CE.
 * Firmware addresses name retained read-only image spans, not upload commands. */
#define BOARD_RADIO_PROFILE_VERSION 3u
#define BOARD_RADIO_PROFILE_SUPERVISED_VERSION 4u
#define BOARD_RADIO_PROFILE_BYTES 1872u
#define BOARD_RADIO_PROFILE_RESPONSE_BYTES_OFFSET 1336u
#define BOARD_RADIO_PROFILE_NETWORK_OFFSET 1856u
#define BOARD_RADIO_PROFILE_MAGIC UINT64_C(0x3152464f52545353)
struct board_radio_profile {
    uint64_t magic;uint32_t version,bytes;
    uint64_t platform_authority,ce_authority,secure_ce_authority,entropy_record;
    uint64_t characterization_record,startup_test_record,health_enable_epoch;
    uint32_t chip_id[4];
    uint8_t platform_evidence[32],ce_evidence[32],entropy_evidence[32];
    struct a733_radio_platform_config platform;
    struct a733_ce_config ce;
    uint64_t firmware_address[4],firmware_bytes[4];
    struct aic8800_radio_identity radio_identity;
    struct aic8800_transport_config transport;
    struct aic8800_identity_admission bootstrap;
    struct aic8800_wifi_config wifi;
    struct aic8800_wifi_target target;
    uint8_t pmk[32],ap_rsn[256];uint16_t ap_rsn_bytes;
    uint8_t address[4],netmask[4],gateway[4];
    uint64_t session_owner,bootstrap_deadline_us,runtime_lease_us,renew_margin_us;
    uint64_t binding_timeout_us,command_timeout_us,refill_timeout_us,tx_timeout_us;
    uint64_t startup_deadline_us,association_timeout_us,security_timeout_us;
    uint32_t queue_timeout_ticks,client_timeout_ticks;
    uint64_t client_deadline_us;
    uint8_t peer_address[4];uint16_t peer_port,request_bytes,response_bytes,reserved;
    uint8_t request[256],response[256];
    _Alignas(8) uint8_t network_dhcp;
    uint8_t network_dns[4],network_reserved[3];
    uint32_t address_timeout_ticks,lease_cap_ticks;
};
/* Wire timing fields are 32-bit ticks, widened into the actual TickType_t.
 * Fixed addressing has no acquisition/lease timer; optional DNS is independent. */
static inline int board_radio_profile_network_valid(const struct board_radio_profile *p)
{
    if(!p||p->network_dhcp>1||p->network_reserved[0]||p->network_reserved[1]||p->network_reserved[2])return 0;
    if(!p->network_dhcp)return !p->address_timeout_ticks&&!p->lease_cap_ticks;
    return p->address_timeout_ticks&&p->lease_cap_ticks&&
        (TickType_t)p->address_timeout_ticks<=portMAX_DELAY/2&&
        (TickType_t)p->lease_cap_ticks<=portMAX_DELAY/2;
}
/* Keep beside the actual shared native_usb provider, outside radio/source
 * storage. Every future cached-pool or CE consumer must share pool_mutex. */
struct board_radio_ce_provider {
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t pool_mutex;
    struct a733_native_usb *shared;
    const struct board_radio_profile *profile;
    struct a733_clock_token token;
    struct a733_ce_entropy *source;
    unsigned used;
};
/* Authenticated profile/span admission occurs in board startup before this
 * constructor. All supplied bytes/objects remain immutable or owned forever. */
int board_radio_startup(struct board_radio *,struct a733_native_usb *,
    struct board_radio_ce_provider *,const void *provider_state,size_t provider_bytes,
    const struct board_radio_profile *,const uint8_t digest[32],const struct board_radio_chip_id *,
    TaskHandle_t owner,TaskHandle_t client,atomic_uint *recovery_state);
/* Successful nonempty controller transactions only; polling, empty status
 * reads and time alone never advance the watchdog's progress input. */
uint64_t board_radio_completed_transfers(const struct board_radio *);
int board_radio_watchdog(struct board_radio *,uint64_t *progress,uint64_t *deadline);
/* Configured client task: one bounded TCP open/connect/send/checked receive/
 * close exchange. Returns0 pending,1 complete,-1 retained failure. */
int board_radio_client_poll(struct board_radio *);
/* Copied terminal status/count for the board's existing result frame. Complete
 * requires every expected byte compared and actual socket CLOSE completion. */
int board_radio_client_result(const struct board_radio *,unsigned *verified_bytes);
/* Owner-only board cutoff. Latches reload refusal before application failure;
 * subsequent owner polls advance only resource/CE retention metadata. */
enum board_radio_result board_radio_external_failure(struct board_radio *);
/* Serialized boot, after shared providers, CE client and real task handles.
 * Fresh retained storage; failed construction consumes this lifetime. */
int board_radio_init(struct board_radio *,const struct board_radio_config *);
/* Same actual radio owner each iteration; existing PMIC task polls its provider. */
enum board_radio_result board_radio_poll(struct board_radio *);
/* Existing copied bounded socket queues; call from configured client tasks. */
int board_radio_submit(struct board_radio *,const struct sunstone_net_request *,uint64_t *);
int board_radio_receive(struct board_radio *,struct sunstone_net_reply *,TickType_t);
#endif
