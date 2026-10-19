#ifndef SUNSTONE_DSI_SESSION_H
#define SUNSTONE_DSI_SESSION_H
#include "boot.h"
#include "profile.h"
struct dsi_boot_contract {struct a733_smp_contract base;uint8_t session_digest[32],entry_digest[32];};
_Static_assert(sizeof(struct dsi_boot_contract)==288,"extended DSI contract");
const uint8_t *dsi_contract_session(const struct a733_smp_contract *);
const uint8_t *dsi_contract_entry(const struct a733_smp_contract *);
int dsi_session_prepare(const struct a733_smp_contract *);
const struct dsi_session *dsi_admitted_session(void);
#endif
