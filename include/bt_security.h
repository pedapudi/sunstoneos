/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_SECURITY_H
#define SUNSTONE_BT_SECURITY_H
#include "bt_acl.h"
#include "bt_security_port.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#define BT_SECURITY_COMMANDS 8u
struct bt_security_command {uint16_t opcode;uint8_t bytes[32];unsigned length;};
struct bt_security_policy {
    uint8_t peer[6],pin[17];
    unsigned allow_just_works,minimum_level;
    uint64_t deadline;
};
struct bt_security {
    const struct bt_security *self;
    struct bt_acl *acl;
    TaskHandle_t owner;
    atomic_uint *recovery;
    struct bt_security_policy policy;
    struct bt_conn connections[BT_ACL_LINKS];
    struct bt_security_command commands[BT_SECURITY_COMMANDS];
    struct net_buf allocation;
    uint64_t firmware_generation,uart_generation,now,tag,command_deadline;
    uint64_t deadline,timeout_us;
    unsigned first,count,allocated,pending,configuration_stage,configured,fatal,entered;
    unsigned pairing_complete,secure_supported,extended_pages;
};
/* One native controller, one task, fixed lifetime RAM keys; no overwrite,
 * persistence, key export, automatic retry or fallback to unencrypted service.
 * Policy copies one actual admitted peer and explicit pairing consent/PIN.
 * Upstream SSP supplies pairing policy; copied command admission does not
 * establish success. Exact tagged completion, authentication, encryption and
 * key-size observations establish service security together. */
int bt_security_init(struct bt_security *,struct bt_acl *,TaskHandle_t,
                     atomic_uint *recovery,const struct bt_security_policy *,uint64_t timeout_us);
/* Attach a reserved actual ACL generation before lower connection polling.
 * Handles are borrowed only from that exact connected slot. No address/handle
 * reuse or key-slot reclamation is admitted during the controller lifetime. */
int bt_security_attach(struct bt_security *,uint64_t acl_generation);
/* One ACL/HCI step, then at most one queued command submission. */
int bt_security_step(struct bt_security *,unsigned budget,uint64_t deadline);
/* Upper HFP/SCO workers already advance ACL: this only pumps copied security
 * command work after their step, never advances HCI a second time. */
int bt_security_service(struct bt_security *,uint64_t deadline);
int bt_security_start(struct bt_security *,uint64_t acl_generation);
/* Read-only owner query; output secure_connections comes actual AES-CCM mode.
 * Generation is the exact authenticated ACL lifetime, not a readiness flag. */
int bt_security_authorized(struct bt_security *,uint64_t acl_generation,uint64_t *generation,unsigned *secure_connections);
/* Metadata-only irreversible revocation; shared recovery closes before fatal
 * publication. Pending controller ownership and key bytes remain retained. */
int bt_security_retain(struct bt_security *);
#endif
