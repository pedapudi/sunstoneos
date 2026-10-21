#ifndef SUNSTONE_AIC8800_NETWORK_H
#define SUNSTONE_AIC8800_NETWORK_H
#include "FreeRTOS.h"
#include "queue.h"
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Routing.h"
#include "aic8800_sdio_transport.h"
#include "aic8800_dispatch.h"
#include <stdatomic.h>
#define AIC8800_NETWORK_TX_SLOTS 8u
struct aic8800_network_config {
    struct aic8800_sdio_transport *transport;
    struct aic8800_dispatch *dispatcher; /* Optional already-claimed bootstrap dispatcher. */
    uint8_t address[4],netmask[4],gateway[4];
    uint64_t tx_timeout; /* Same counter units as the admitted radio transport. */
    TickType_t queue_timeout;
    /* Exact 1 on success; production cryptographic entropy. random32 may return
     * 0 for a temporary refusal; any other failure closes output. */
    int (*random32)(void *,uint32_t *);
    int (*sequence)(void *,uint32_t,uint16_t,uint32_t,uint16_t,uint32_t *);
    void *entropy_context;
    uint64_t entropy_record;
    /* Zero preserves fixed IPv4. DHCP never falls back to fixed defaults. */
    uint8_t dhcp,dns[4];
    TickType_t address_timeout; /* Finite acquisition bound; <= portMAX_DELAY/2. */
    TickType_t lease_cap; /* Conservative maximum lifetime of each acknowledged lease. */
};
struct aic8800_network_packet {uint64_t generation;TickType_t queued;uint16_t bytes;uint8_t ethernet[1514];};
struct aic8800_network {
    struct aic8800_network_config config;
    struct aic8800_dispatch local_dispatcher;
    struct aic8800_dispatch *receive;
    NetworkInterface_t interface;
    NetworkEndPoint_t endpoint;
    QueueHandle_t tx;
    StaticQueue_t tx_queue;
    uint8_t tx_storage[AIC8800_NETWORK_TX_SLOTS*sizeof(struct aic8800_network_packet)];
    struct aic8800_network_packet current;
    uint64_t generation,rx_packets,tx_bus_completions,rx_dropped,tx_dropped;
    atomic_uint ready,failed,address_ready,address_failed,address_sequence;
    atomic_uint offline_request,offline_ack,offline,resume_request;
    unsigned offline_queued; /* Sole IP task; private FIFO marker identity below. */
    uint8_t offline_marker;
    atomic_uint_least32_t address_value,mask_value,gateway_value,dns_value;
    atomic_uint_fast64_t address_started,lease_started,lease_ticks;
    uint32_t leased_address; /* IP task: fixed within each online DHCP lifetime. */
    uint8_t initialized,owns_current,active,down_sent,stack_started;
};
/* One stack instance for its image lifetime. Fresh storage; sole radio task
 * calls activate/poll. Stack callbacks only enqueue copied packets. Contexts,
 * transport and config spans must remain private and separate. No auto reinit.
 * Activation requires idle DATA and no outstanding SDIO/security TX. Any held
 * RX record transfers with the transport. The native dispatcher copies events
 * and retires unclaimed records; ordinary Ethernet stays with this adapter. */
int aic8800_network_storage_valid(const void *,size_t,const struct aic8800_network_config *);
int aic8800_network_activate(struct aic8800_network *,const struct aic8800_network_config *);
void aic8800_network_poll(struct aic8800_network *);
void aic8800_network_fault(struct aic8800_network *);
int aic8800_network_ready(const struct aic8800_network *);
/* IP-task-only lease checks, called by the existing stack event trace hook. */
void aic8800_network_ip_step(void);
/* Healthy, quiescent lifetime only. IP-task receipt follows actual down
 * processing and every earlier FIFO event. No failed flag is cleared. */
void aic8800_network_ip_event(unsigned,void *);
int aic8800_network_offline(struct aic8800_network *);
int aic8800_network_offline_ready(struct aic8800_network *);
int aic8800_network_resume(struct aic8800_network *);
int aic8800_network_address(const struct aic8800_network *,uint8_t [4]);
int aic8800_network_status(const struct aic8800_network *,uint32_t [4]);
#endif
