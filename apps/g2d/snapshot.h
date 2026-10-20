#ifndef SUNSTONE_G2D_SNAPSHOT_H
#define SUNSTONE_G2D_SNAPSHOT_H
#include "a733_g2d_application.h"
#include "boot-record.h"
int g2d_session_prepare(const struct a733_smp_contract *);
const struct a733_g2d_session *g2d_admitted_session(void);
const uint8_t *g2d_admitted_session_digest(void);
#endif
