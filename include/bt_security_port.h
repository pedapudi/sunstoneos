/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BT_SECURITY_PORT_H
#define SUNSTONE_BT_SECURITY_PORT_H
/* Fixed, owner-task-only compatibility surface for the pinned Zephyr SSP/key
 * workers. No Zephyr scheduler, settings store, duplicate HCI or heap owner. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#define __fallthrough __attribute__((fallthrough))
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif
#define CONFIG_BT_MAX_PAIRED 4
#define CONFIG_BT_SMP_SC_ONLY 0
#define BT_PORT_PLACEHOLDER_1 0,
#define BT_PORT_SECOND(ignore,value,...) value
#define BT_PORT_DEFINED_VALUE(value) BT_PORT_DEFINED_TOKEN(BT_PORT_PLACEHOLDER_##value)
#define BT_PORT_DEFINED_TOKEN(token) BT_PORT_SECOND(token 1,0)
#define IS_ENABLED(value) BT_PORT_DEFINED_VALUE(value)
static inline char *bt_port_strncpy(char *out,const char *in,size_t n)
{size_t i=0;for(;i<n&&in[i];++i)out[i]=in[i];for(;i<n;++i)out[i]=0;return out;}
#define strncpy bt_port_strncpy
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define __ASSERT_NO_MSG(x) assert(x)
#define K_FOREVER (-1)
typedef struct {uint8_t val[6];} bt_addr_t;
typedef struct {uint8_t type;bt_addr_t a;} bt_addr_le_t;
#define BT_ADDR_LE_PUBLIC 0
extern const bt_addr_t bt_security_any_address;
#define BT_ADDR_ANY (&bt_security_any_address)
static inline bool bt_addr_eq(const bt_addr_t *a,const bt_addr_t *b){return !memcmp(a,b,sizeof(*a));}
static inline void bt_addr_copy(bt_addr_t *a,const bt_addr_t *b){memcpy(a,b,sizeof(*a));}
static inline uint16_t sys_cpu_to_le16(uint16_t x){return x;}
static inline uint16_t sys_le16_to_cpu(uint16_t x){return x;}
static inline uint32_t sys_cpu_to_le32(uint32_t x){return x;}
static inline uint32_t sys_le32_to_cpu(uint32_t x){return x;}
#include "zephyr-security-wire.h"
/* Source key flags, keys.h enum bt_keys_link_key_flags. */
#define BT_LINK_KEY_AUTHENTICATED 1u
#define BT_LINK_KEY_DEBUG 2u
#define BT_LINK_KEY_SC 4u
struct bt_keys_link_key {bt_addr_t addr;uint8_t storage_start[0] __attribute__((aligned(sizeof(void *))));uint8_t flags,val[16];};
#define BT_KEYS_LINK_KEY_STORAGE_LEN (sizeof(struct bt_keys_link_key)-offsetof(struct bt_keys_link_key,storage_start))
struct bt_br_bond_info {bt_addr_t addr;};
enum bt_security_err {BT_SECURITY_ERR_SUCCESS=0,BT_SECURITY_ERR_AUTHENTICATION=1};
typedef unsigned bt_security_t;
enum {BT_SECURITY_L1=1,BT_SECURITY_L2,BT_SECURITY_L3,BT_SECURITY_L4};
enum {BT_CONN_BR_LEGACY_SECURE,BT_CONN_BR_BONDABLE,BT_CONN_USER,BT_CONN_BR_PAIRING,
    BT_CONN_BR_PAIRED,BT_CONN_BR_NOBOND,BT_CONN_BR_GENERAL_BONDING,BT_CONN_BR_PAIRING_INITIATOR};
#define BT_CONN_TYPE_BR 1
static inline void atomic_set_bit(uint64_t *p,unsigned n){*p|=UINT64_C(1)<<n;}
static inline void atomic_clear_bit(uint64_t *p,unsigned n){*p&=~(UINT64_C(1)<<n);}
static inline bool atomic_test_bit(const uint64_t *p,unsigned n){return !!(*p&(UINT64_C(1)<<n));}
static inline bool atomic_test_and_clear_bit(uint64_t *p,unsigned n){bool old=atomic_test_bit(p,n);atomic_clear_bit(p,n);return old;}
static inline void atomic_set_bit_to(uint64_t *p,unsigned n,bool value){if(value)atomic_set_bit(p,n);else atomic_clear_bit(p,n);}
struct bt_conn {
    uint64_t flags[1],acl_generation,firmware_generation,uart_generation;
    uint16_t handle;
    unsigned encrypt,sec_level,required_sec_level,key_size,key_verified,references,attached,authenticated;
    struct {bt_addr_t dst;struct bt_keys_link_key *link_key;uint8_t remote_io_capa,remote_auth,local_auth,pairing_method;} br;
};
struct bt_conn_auth_cb {
    void (*passkey_confirm)(struct bt_conn *,unsigned);
    void (*passkey_display)(struct bt_conn *,unsigned);
    void (*passkey_entry)(struct bt_conn *);
    void (*pincode_entry)(struct bt_conn *,bool);
    void (*pairing_confirm)(struct bt_conn *);
};
struct bt_conn_auth_info_cb {void (*pairing_complete)(struct bt_conn *,bool);void (*pairing_failed)(struct bt_conn *,enum bt_security_err);};
extern const struct bt_conn_auth_cb *bt_auth;
#define SYS_SLIST_FOR_EACH_CONTAINER_SAFE(list,item,next,node) for((item)=NULL,(next)=NULL;(item)!=NULL;(void)(next))
struct net_buf {uint8_t *data;size_t len;uint8_t bytes[32];};
struct net_buf *bt_hci_cmd_alloc(int);
void *net_buf_add(struct net_buf *,size_t);
int bt_hci_cmd_send_sync(uint16_t,struct net_buf *,struct net_buf **);
struct bt_conn *bt_conn_lookup_addr_br(const bt_addr_t *);
struct bt_conn *bt_conn_lookup_handle(uint16_t,unsigned);
void bt_conn_unref(struct bt_conn *);
int bt_conn_disconnect(struct bt_conn *,uint8_t);
bool bt_conn_is_type(struct bt_conn *,unsigned);
void bt_conn_security_changed(struct bt_conn *,uint8_t,enum bt_security_err);
enum bt_security_err bt_security_err_get(uint8_t);
unsigned bt_l2cap_br_get_max_sec_level(void);
bool br_sufficient_key_size(struct bt_conn *);
bool bt_br_update_sec_level(struct bt_conn *);
struct bt_keys_link_key *bt_keys_get_link_key(const bt_addr_t *);
struct bt_keys_link_key *bt_keys_find_link_key(const bt_addr_t *);
void bt_keys_link_key_clear(struct bt_keys_link_key *);
void bt_keys_link_key_store(struct bt_keys_link_key *);
static inline int bt_settings_delete_link_key(const bt_addr_le_t *a){(void)a;return -ENOTSUP;}
static inline int bt_settings_store_link_key(const bt_addr_le_t *a,const void *p,size_t n){(void)a;(void)p;(void)n;return -ENOTSUP;}
int bt_ssp_start_security(struct bt_conn *);
int bt_conn_auth_pincode_entry(struct bt_conn *,const char *);
int bt_ssp_auth_pairing_confirm(struct bt_conn *);
int bt_ssp_auth_cancel(struct bt_conn *);
void bt_hci_pin_code_req(struct net_buf *);
void bt_hci_link_key_notify(struct net_buf *);
void bt_hci_link_key_req(struct net_buf *);
void bt_hci_io_capa_resp(struct net_buf *);
void bt_hci_io_capa_req(struct net_buf *);
void bt_hci_ssp_complete(struct net_buf *);
void bt_hci_user_confirm_req(struct net_buf *);
void bt_hci_user_passkey_notify(struct net_buf *);
void bt_hci_user_passkey_req(struct net_buf *);
void bt_hci_auth_complete(struct net_buf *);
#endif
