#ifndef SUNSTONE_DISPLAYPORT_SNAPSHOT_H
#define SUNSTONE_DISPLAYPORT_SNAPSHOT_H
#include "a733_displayport_application.h"
#include "boot-record.h"
int displayport_session_prepare(const struct a733_smp_contract *);
const struct a733_displayport_session *displayport_admitted_session(void);
const uint8_t *displayport_admitted_session_digest(void);
#endif
