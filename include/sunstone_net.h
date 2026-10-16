#ifndef SUNSTONE_NET_H
#define SUNSTONE_NET_H
#include "aic8800_network.h"
#include "FreeRTOS_Sockets.h"
#include "task.h"
#include <stdatomic.h>
#define SUNSTONE_NET_CLIENTS 4u
#define SUNSTONE_NET_SOCKETS 4u
#define SUNSTONE_NET_BYTES 1472u
#define SUNSTONE_NET_STACK_WORDS 4096u
enum sunstone_net_operation { SUNSTONE_NET_OPEN_UDP,SUNSTONE_NET_OPEN_TCP,SUNSTONE_NET_CONNECT,
    SUNSTONE_NET_SEND,SUNSTONE_NET_RECEIVE,SUNSTONE_NET_CLOSE,SUNSTONE_NET_EVENT };
enum sunstone_net_result { SUNSTONE_NET_OK,SUNSTONE_NET_INVALID,SUNSTONE_NET_BUSY,SUNSTONE_NET_TIMEOUT,
    SUNSTONE_NET_IO,SUNSTONE_NET_LINK_LOST };
struct sunstone_net_request {
    enum sunstone_net_operation operation;
    uint64_t handle;
    TickType_t timeout;
    uint8_t address[4];uint16_t port,bytes;
    uint8_t payload[SUNSTONE_NET_BYTES];
};
struct sunstone_net_reply {
    enum sunstone_net_result result;
    uint64_t ticket,handle;
    int stack_result;
    struct aic8800_dispatch_event event; /* Filled only for EVENT replies. */
    uint16_t bytes,port;uint8_t address[4];
    uint8_t payload[SUNSTONE_NET_BYTES];
};
struct sunstone_net_message {struct sunstone_net_request request;uint64_t ticket;unsigned client;TickType_t submitted;};
struct sunstone_net_client {
    TaskHandle_t task;
    uint64_t next_ticket,pending;
    QueueHandle_t reply_queue;StaticQueue_t queue;
    uint8_t storage[sizeof(struct sunstone_net_reply)];
};
struct sunstone_net_socket {
    Socket_t socket;
    uint64_t handle;
    unsigned client;
    atomic_uint disposed;
    atomic_uintptr_t disposal_address;
    uint8_t tcp,bound,closing,uncertain;
};
struct sunstone_net_pending {struct sunstone_net_message message;unsigned slot;uint8_t active,started;};
struct sunstone_net_config {
    struct aic8800_network_config network;
    TaskHandle_t bootstrap,clients[SUNSTONE_NET_CLIENTS];
};
struct sunstone_net {
    struct aic8800_network network;
    struct sunstone_net_config config;
    struct sunstone_net_client clients[SUNSTONE_NET_CLIENTS];
    struct sunstone_net_socket sockets[SUNSTONE_NET_SOCKETS];
    struct sunstone_net_pending pending[SUNSTONE_NET_CLIENTS];
    QueueHandle_t requests;StaticQueue_t request_queue;
    uint8_t request_storage[SUNSTONE_NET_CLIENTS*sizeof(struct sunstone_net_message)];
    StaticTask_t task;StackType_t stack[SUNSTONE_NET_STACK_WORDS];TaskHandle_t worker;
    uint64_t next_handle;
    atomic_uint activation,failed;
    uint8_t initialized,owned_poll;
};
/* Trusted initialization before scheduling. Configuration and task identities
 * are copied; radio and entropy contexts remain pinned. Activation transfers
 * exclusive radio ownership from the declared bootstrap task to this worker.
 * No API call exposes stack socket pointers or borrowed application buffers.
 * EVENT copies one ordered radio event, including after link revocation. A late
 * reply preserves that copied event with TIMEOUT; receipt does not authorize RF. */
int sunstone_net_init(struct sunstone_net *,const struct sunstone_net_config *);
/* Running-task admission for a native radio session. The current task must be
 * config.bootstrap and remains the only radio/socket polling owner. Creates
 * the same bounded queues without creating another worker or transferring
 * radio ownership. All storage is retained through failure. */
int sunstone_net_init_owned(struct sunstone_net *,const struct sunstone_net_config *);
/* One nonblocking network/socket iteration on the recorded task. The owning
 * session must authenticate its actual platform lifetime before each call;
 * after revocation, fault the network before calling to drain copied replies. */
int sunstone_net_poll_owned(struct sunstone_net *);
int sunstone_net_activate(struct sunstone_net *);
int sunstone_net_submit(struct sunstone_net *,const struct sunstone_net_request *,uint64_t *ticket);
int sunstone_net_receive(struct sunstone_net *,struct sunstone_net_reply *,TickType_t wait);
/* FreeRTOS+TCP's supported socket-free override; invoked after its socket's
 * buffers/event group are disposed. Applications must not call it. */
void sunstone_net_stack_free(void *);
#endif
