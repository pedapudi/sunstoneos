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
    int (*random32)(void *,uint32_t *); /* Exact 1; production cryptographic entropy. */
    int (*sequence)(void *,uint32_t,uint16_t,uint32_t,uint16_t,uint32_t *);
    void *entropy_context;
    uint64_t entropy_record;
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
    atomic_uint ready,failed;
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
#endif
